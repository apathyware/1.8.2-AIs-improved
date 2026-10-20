#include "pch.h"
#include "../Public/Client.h"
#include "../../../Four_E_Zero_H/Four_E_Zero_H/Public/Configuration.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlaylistAthena.h"
#include "../../../Four_E_Zero_H/Four_E_Zero_H/Public/NoKillerMessages.h"
#include <string>
#include <atomic>
#include <vector>
#include <cwctype>

bool bEOREnabled = true;
bool bDisablePreEdits = false;
inline void* (*SelectResetOG)(void*) = nullptr;
inline void* (*SelectEditOG)(void*) = nullptr;
inline void (*PerformBuildingEditInteractionOG)(void*) = nullptr;
inline void (*CompleteBuildingEditInteraction)(void*) = nullptr;

void* SelectEdit(void* a1)
{
    void* result = SelectEditOG(a1);

    if (bEOREnabled && CompleteBuildingEditInteraction)
        CompleteBuildingEditInteraction(a1);

    return result;
}

void* SelectReset(void* a1)
{
    void* result = SelectResetOG(a1);

    if (bEOREnabled && CompleteBuildingEditInteraction)
        CompleteBuildingEditInteraction(a1);

    return result;
}
void PerformBuildingEditInteraction(AFortPlayerControllerAthena* _this)
{
    if (bDisablePreEdits && _this->TargetedBuilding->IsA<ABuildingPlayerPrimitivePreview>())
        return;

    return PerformBuildingEditInteractionOG(_this);
}

void JoinLocalServer()
{
    auto World = UWorld::GetWorld();

    if (!World)
        return;

    std::wstring Command = L"open 127.0.0.1:" + std::to_wstring((int)FConfiguration::Port);

    UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(Command.c_str()), nullptr);
}

static std::atomic<bool> GTurboBuild{ false };   
static std::atomic<int> GTurboToggles{ 0 };      
static std::atomic<DWORD> GGameThreadId{ 0 };    

static void (*ProcessEventOG)(UObject*, UFunction*, void*) = nullptr;
static int GInOurCode = 0;                       

static bool GTurboBroken = false;
static bool GKillFeedBroken = false;
static bool GReportsBroken = false;
static bool GStormReportBroken = false;
static int GFaults = 0;                          
static bool GFaultToReport = false;              

static FName GNameOnRepDeathInfo;
static FName GNameUpdateKillFeed;
static FName GNameClientReportKill;
static FName GNameServerCreateBuildingActor;

struct FGameWindow
{
    HWND Window = nullptr;
    DWORD Thread = 0;
};

static BOOL CALLBACK FindGameWindow(HWND Window, LPARAM Out)
{
    DWORD Pid = 0;
    const DWORD Thread = GetWindowThreadProcessId(Window, &Pid);

    if (Pid != GetCurrentProcessId())
        return TRUE;

    char ClassName[64] = {};
    GetClassNameA(Window, ClassName, sizeof(ClassName));

    if (strcmp(ClassName, "UnrealWindow") != 0)
        return TRUE;

    ((FGameWindow*)Out)->Window = Window;
    ((FGameWindow*)Out)->Thread = Thread;
    return FALSE;
}

static bool GameHasFocus()
{
    DWORD Pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &Pid);
    return Pid == GetCurrentProcessId();
}

static AFortPlayerControllerAthena* LocalController()
{
    auto World = UWorld::GetWorld();

    if (!World || !World->OwningGameInstance)
        return nullptr;

    auto& LocalPlayers = World->OwningGameInstance->LocalPlayers;

    if (LocalPlayers.Num() <= 0 || !LocalPlayers[0])
        return nullptr;

    return (AFortPlayerControllerAthena*)LocalPlayers[0]->PlayerController;
}

static std::wstring OneDecimal(double Value)
{
    wchar_t Out[48];
    swprintf(Out, 48, L"%.1f", Value);
    return std::wstring(Out);
}

static void TellServer(const std::wstring& Text)
{
    static UFunction* Fn = nullptr;
    static uint32 Offset = 0;
    static uint32 Size = 0;
    static bool bLooked = false;
    static double Tokens = 12.0;
    static ULONGLONG LastRefill = 0;

    if (GReportsBroken)
        return;

    auto World = UWorld::GetWorld();
    if (!World || !World->NetDriver)
        return;

    auto Controller = LocalController();
    if (!Controller)
        return;

    if (!bLooked)
    {
        bLooked = true;
        Fn = Controller->GetFunction("ServerCheat");

        if (Fn)
        {
            auto Params = Fn->GetParamsNamed();
            Size = Params.Size;

            if (!Params.NameOffsetMap.empty())
                Offset = Params.NameOffsetMap[0].Offset;
        }
    }

    if (!Fn || Size == 0 || Size > 0x100 || Offset + 0x10 > Size)
        return;

    const ULONGLONG Now = GetTickCount64();

    if (LastRefill == 0)
        LastRefill = Now;

    Tokens += (double)(Now - LastRefill) / 1000.0 * 6.0;
    LastRefill = Now;

    if (Tokens > 12.0)
        Tokens = 12.0;

    if (Tokens < 1.0)
        return;

    Tokens -= 1.0;

    std::wstring Line = L"clientlog " + Text;

    if (Line.size() > 400)
        Line.resize(400);

    FString Param(Line.c_str());

    alignas(16) uint8 Buffer[0x110] = {};
    memcpy(Buffer + Offset, (const void*)&Param, 0x10);

    Controller->ProcessEvent(Fn, Buffer);

    if (Param.Data)
        FMemory::Free(Param.Data);
}

static void SayInConsole(const std::wstring& Text)
{
    static UFunction* Fn = nullptr;
    static uint32 Offset = 0;
    static uint32 Size = 0;
    static bool bLooked = false;

    auto Controller = LocalController();
    if (!Controller)
        return;

    if (!bLooked)
    {
        bLooked = true;
        Fn = Controller->GetFunction("ClientMessage");

        if (Fn)
        {
            auto Params = Fn->GetParamsNamed();
            Size = Params.Size;

            if (!Params.NameOffsetMap.empty())
                Offset = Params.NameOffsetMap[0].Offset;
        }
    }

    if (!Fn || Size == 0 || Size > 0x100 || Offset + 0x10 > Size)
        return;

    FString Param(Text.c_str());

    alignas(16) uint8 Buffer[0x110] = {};
    memcpy(Buffer + Offset, (const void*)&Param, 0x10);

    Controller->ProcessEvent(Fn, Buffer);

    if (Param.Data)
        FMemory::Free(Param.Data);
}

static int32 ReflectedOffset(const UObject* Object, const char* Name)
{
    if (!Object)
        return -1;

    const uint32 Offset = Object->GetOffset(Name);
    return Offset == (uint32)-1 ? -1 : (int32)Offset;
}

static std::wstring PlayerNameOf(const UObject* PlayerState)
{
    static int32 Offset = -2;

    if (!PlayerState)
        return std::wstring();

    if (Offset == -2)
    {
        Offset = ReflectedOffset(PlayerState, "PlayerNamePrivate");

        if (Offset < 0)
            Offset = ReflectedOffset(PlayerState, "PlayerName");
    }

    if (Offset < 0)
        return std::wstring();

    auto& Name = GetFromOffset<FString>(PlayerState, (uint32)Offset);

    if (!Name.CStr() || Name.Num() <= 1)
        return std::wstring();

    return std::wstring(Name.CStr());
}

struct FTurboBuild
{
    ULONGLONG HoldStart = 0;      
    ULONGLONG LastSend = 0;
    const void* LastClass = nullptr;
    float LastLoc[3] = {};
    float LastRot[3] = {};
    bool bLastMirrored = false;
    int SentThisHold = 0;
    int SpotsThisHold = 0;
    int SentTotal = 0;
    int32 PreviousLevel = 0;      
    int32 UpgradeLevel = 0;
};

static FTurboBuild GTurbo;

static UFunction* GCreateBuildingFn = nullptr;
static int32 GCreateDataOffset = -1, GCreateLocOffset = -1, GCreateRotOffset = -1, GCreateMirrorOffset = -1;
static uint32 GCreateParamsSize = 0;
static bool GCreateLooked = false, GCreateUsable = false;

static void FindCreateBuildingParams(AFortPlayerControllerAthena* Controller)
{
    if (GCreateLooked || !Controller)
        return;

    GCreateLooked = true;
    GCreateBuildingFn = Controller->GetFunction("ServerCreateBuildingActor");

    if (GCreateBuildingFn)
    {
        auto Params = GCreateBuildingFn->GetParamsNamed();
        GCreateParamsSize = Params.Size;

        for (auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "BuildingClassData")
                GCreateDataOffset = (int32)Param.Offset;
            else if (Param.Name == "BuildLoc")
                GCreateLocOffset = (int32)Param.Offset;
            else if (Param.Name == "BuildRot")
                GCreateRotOffset = (int32)Param.Offset;
            else if (Param.Name == "bMirrored")
                GCreateMirrorOffset = (int32)Param.Offset;
        }
    }

    GCreateUsable = GCreateBuildingFn && GCreateDataOffset >= 0 && GCreateLocOffset >= 0 && GCreateRotOffset >= 0 &&
                    GCreateMirrorOffset >= 0 && GCreateParamsSize > 0 && GCreateParamsSize <= 0x100;

    if (!GCreateUsable)
        TellServer(L"F7 turbo building: this build's ServerCreateBuildingActor does not take (BuildingClassData, BuildLoc, BuildRot, "
                   L"bMirrored) -- turbo building cannot work on it.");
}

static bool InBuildMode(AFortPlayerControllerAthena* Controller)
{
    static UFunction* Fn = nullptr;
    static bool bLooked = false;

    if (!bLooked)
    {
        bLooked = true;
        Fn = Controller->GetFunction("IsInBuildMode");
    }

    return Fn && Controller->Call<bool>(Fn);
}

static void TurboBuildTick(ULONGLONG Now)
{
    const bool bOn = GTurboBuild.load() && !GTurboBroken;
    const bool bHeld = bOn && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && GameHasFocus();

    if (!bHeld)
    {
        if (GTurbo.HoldStart && GTurbo.SentThisHold > 0)
            TellServer(L"F7 turbo building: held the button " + OneDecimal((double)(Now - GTurbo.HoldStart) / 1000.0) +
                       L"s -- sent " + std::to_wstring(GTurbo.SentThisHold) + L" placement(s) to " + std::to_wstring(GTurbo.SpotsThisHold) +
                       L" different spot(s) (" + std::to_wstring(GTurbo.SentTotal) + L" this session); the server's [BUILD-LOG] lines say "
                       L"which it built.");

        GTurbo.HoldStart = 0;
        GTurbo.SentThisHold = 0;
        GTurbo.SpotsThisHold = 0;
        GTurbo.LastClass = nullptr;
        return;
    }

    if (!GTurbo.HoldStart)
        GTurbo.HoldStart = Now;

    if (Now - GTurbo.HoldStart < 150 || Now - GTurbo.LastSend < 60)
        return;

    auto Controller = LocalController();
    if (!Controller)
        return;

    FindCreateBuildingParams(Controller);

    if (!GCreateUsable || !InBuildMode(Controller))
        return;

    static int32 EditOffset = -2;

    if (EditOffset == -2)
        EditOffset = ReflectedOffset(Controller, "EditBuildingActor");

    if (EditOffset >= 0 && GetFromOffset<UObject*>(Controller, (uint32)EditOffset))
        return;

    auto Preview = (AActor*)Controller->BuildPreviewMarker;
    if (!Preview || ((ABuildingPlayerPrimitivePreview*)Preview)->bHidden)
        return;

    static int32 ClassOffset = -2;

    if (ClassOffset == -2)
        ClassOffset = ReflectedOffset(Controller, "CurrentBuildableClass");

    if (ClassOffset < 0)
        return;

    UClass* Class = GetFromOffset<UClass*>(Controller, (uint32)ClassOffset);
    if (!Class)
        return;

    const FVector Location = Preview->K2_GetActorLocation();
    const FRotator Rotation = Preview->K2_GetActorRotation();
    const bool bMirrored = Controller->bBuildPreviewMirrored;

    float Loc[3] = { (float)Location.X, (float)Location.Y, (float)Location.Z };
    float Rot[3] = { (float)Rotation.Pitch, (float)Rotation.Yaw, (float)Rotation.Roll };

    const bool bSameSpot = GTurbo.LastClass == (const void*)Class && GTurbo.bLastMirrored == bMirrored &&
                           memcmp(GTurbo.LastLoc, Loc, sizeof(Loc)) == 0 && memcmp(GTurbo.LastRot, Rot, sizeof(Rot)) == 0;

    if (bSameSpot && Now - GTurbo.LastSend < 400)
        return;

    alignas(16) uint8 Buffer[0x110] = {};

    *(UClass**)(Buffer + GCreateDataOffset) = Class;
    *(int32*)(Buffer + GCreateDataOffset + 8) = GTurbo.PreviousLevel;
    *(int32*)(Buffer + GCreateDataOffset + 12) = GTurbo.UpgradeLevel;
    memcpy(Buffer + GCreateLocOffset, Loc, sizeof(Loc));
    memcpy(Buffer + GCreateRotOffset, Rot, sizeof(Rot));
    Buffer[GCreateMirrorOffset] = bMirrored ? 1 : 0;

    Controller->ProcessEvent(GCreateBuildingFn, Buffer);

    if (!bSameSpot)
        GTurbo.SpotsThisHold++;

    GTurbo.LastSend = Now;
    GTurbo.LastClass = (const void*)Class;
    GTurbo.bLastMirrored = bMirrored;
    memcpy(GTurbo.LastLoc, Loc, sizeof(Loc));
    memcpy(GTurbo.LastRot, Rot, sizeof(Rot));
    GTurbo.SentThisHold++;
    GTurbo.SentTotal++;
}

struct FNoKillerDeath
{
    std::wstring Name;
    ULONGLONG At = 0;
};

static std::vector<FNoKillerDeath> GNoKillerDeaths;
static std::vector<const void*> GKillFeedSeen;   
static UObject* GKillFeedWidget = nullptr;
static int32 GKillFeedWidgetIndex = -1;

static int32 GDeathInfoOffset = -2, GFinisherOffset = -2, GDeathCauseOffset = -2, GKillFeedOffset = -2;

static void OnDeathInfoArrived(UObject* PlayerState)
{
    if (GDeathInfoOffset == -2)
    {
        GDeathInfoOffset = ReflectedOffset(PlayerState, "DeathInfo");

        auto Struct = FindStruct("DeathInfo");
        const uint32 Finisher = Struct ? Struct->GetOffset("FinisherOrDowner") : (uint32)-1;
        const uint32 Cause = Struct ? Struct->GetOffset("DeathCause") : (uint32)-1;

        GFinisherOffset = Finisher == (uint32)-1 ? -1 : (int32)Finisher;
        GDeathCauseOffset = Cause == (uint32)-1 ? -1 : (int32)Cause;
    }

    if (GDeathInfoOffset < 0 || GFinisherOffset < 0)
        return;

    auto Finisher = GetFromOffset<UObject*>(PlayerState, (uint32)(GDeathInfoOffset + GFinisherOffset));

    if (Finisher && Finisher != PlayerState)
        return;   

    const std::wstring Name = PlayerNameOf(PlayerState);
    if (Name.empty())
        return;

    GNoKillerDeaths.push_back({ Name, GetTickCount64() });
}

static FText MakeText(const std::wstring& Text)
{
    return UKismetTextLibrary::Conv_StringToText(FString(Text.c_str()));
}

static std::wstring TextToWide(FText& Text)
{
    FString String = UKismetTextLibrary::Conv_TextToString(Text);

    std::wstring Wide = (String.CStr() && String.Num() > 0) ? std::wstring(String.CStr()) : std::wstring();

    if (String.Data)
        FMemory::Free(String.Data);

    return Wide;
}

static bool LineNames(const std::wstring& Line, const std::wstring& Name)
{
    auto IsWordChar = [](wchar_t Ch) { return iswalnum(Ch) || Ch == L'_'; };

    for (size_t At = Line.find(Name); At != std::wstring::npos; At = Line.find(Name, At + 1))
    {
        const bool bStartOk = At == 0 || !IsWordChar(Line[At - 1]);
        const bool bEndOk = At + Name.size() >= Line.size() || !IsWordChar(Line[At + Name.size()]);

        if (bStartOk && bEndOk)
            return true;
    }

    return false;
}

static bool LookAtTheKillFeed()
{
    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return false;

    auto GameState = (UObject*)World->GameState;

    static const void* KillFeedClass = nullptr;

    if (GKillFeedOffset == -2 || (const void*)GameState->Class != KillFeedClass)
    {
        KillFeedClass = (const void*)GameState->Class;
        GKillFeedOffset = ReflectedOffset(GameState, "KillFeedEntry");
        GKillFeedSeen.clear();

        if (GKillFeedOffset < 0 && GameState->Class && GameState->Class->Name.ToString().find("Athena") != UEAllocatedString::npos)
            TellServer(L"killfeed: this build's game state has no KillFeedEntry -- the \"no killer\" lines cannot go in the killfeed.");
    }

    if (GKillFeedOffset < 0)
        return false;

    auto& Lines = GetFromOffset<TArray<FText>>(GameState, (uint32)GKillFeedOffset);

    if (Lines.Num() <= 0 || Lines.Num() > 64 || !Lines.Data)
    {
        GKillFeedSeen.clear();
        return false;
    }

    const ULONGLONG Now = GetTickCount64();
    bool bSwapped = false;
    std::vector<const void*> StillThere;

    for (int i = 0; i < Lines.Num(); i++)
    {
        FText& Line = Lines.Data[i];
        const void* Key = *(const void* const*)&Line;

        if (!Key)
            continue;

        if (std::find(GKillFeedSeen.begin(), GKillFeedSeen.end(), Key) != GKillFeedSeen.end())
        {
            StillThere.push_back(Key);
            continue;
        }

        const std::wstring Native = TextToWide(Line);
        std::wstring Shown = Native;
        std::wstring Note;

        for (size_t d = 0; d < GNoKillerDeaths.size(); d++)
        {
            const auto& Death = GNoKillerDeaths[d];

            if (!LineNames(Native, Death.Name))
                continue;

            Shown = NoKillerMessages::LineForKillFeed(NoKillerMessages::PickFor(Death.Name.c_str()), Death.Name);

            FText NewText = MakeText(Shown);
            memcpy((void*)&Line, (const void*)&NewText, sizeof(FText));

            Key = *(const void* const*)&Line;
            Note = L" (the game's own line was \"" + Native + L"\" -- swapped: nobody got this elim)";

            GNoKillerDeaths.erase(GNoKillerDeaths.begin() + (ptrdiff_t)d);
            bSwapped = true;
            break;
        }

        StillThere.push_back(Key);
        TellServer(L"killfeed line on this screen: \"" + Shown + L"\"" + Note);
    }

    GKillFeedSeen.swap(StillThere);

    for (size_t d = 0; d < GNoKillerDeaths.size();)
    {
        if (Now - GNoKillerDeaths[d].At > 4000)
        {
            TellServer(L"killfeed: no line naming '" + GNoKillerDeaths[d].Name + L"' turned up in 4s, so no \"no killer\" line could go in.");
            GNoKillerDeaths.erase(GNoKillerDeaths.begin() + (ptrdiff_t)d);
        }
        else
        {
            d++;
        }
    }

    return bSwapped;
}

static void RefreshKillFeedWidget()
{
    if (GKillFeedWidget && GKillFeedWidgetIndex >= 0)
    {
        auto Item = TUObjectArray::GetItemByIndex(GKillFeedWidgetIndex);

        if (!Item || Item->Object != GKillFeedWidget)
            GKillFeedWidget = nullptr;
    }

    if (!GKillFeedWidget)
    {
        GKillFeedWidget = (UObject*)TUObjectArray::FindFirstObject("AthenaKillFeedWidget_C");
        GKillFeedWidgetIndex = GKillFeedWidget ? GKillFeedWidget->Index : -1;
    }

    if (!GKillFeedWidget)
        return;

    if (auto Fn = GKillFeedWidget->GetFunction("UpdateKillFeed"))
        GKillFeedWidget->ProcessEvent(Fn, nullptr);
}

static void OnClientReportKill(UFunction* Function, void* Params)
{
    static int Kind = -1;   
    static uint32 Offset = 0;

    if (Kind == -1)
    {
        Kind = 2;
        auto Named = Function->GetParamsNamed();

        if (!Named.NameOffsetMap.empty())
        {
            Offset = Named.NameOffsetMap[0].Offset;
            Kind = Named.NameOffsetMap[0].ElementSize == 0x10 ? 0 : (Named.NameOffsetMap[0].ElementSize == 8 ? 1 : 2);
        }
    }

    if (!Params || Kind == 2)
        return;

    std::wstring Name;

    if (Kind == 0)
    {
        auto& Given = *(FString*)((uint8*)Params + Offset);

        if (Given.CStr() && Given.Num() > 1)
            Name = std::wstring(Given.CStr());
    }
    else
    {
        Name = PlayerNameOf(*(UObject**)((uint8*)Params + Offset));
    }

    TellServer(Name.empty() ? std::wstring(L"popup on this screen: \"Eliminated\" with NO name")
                            : (L"popup on this screen: \"Eliminated " + Name + L"\""));
}

static void ReportStorm()
{
    struct FSeen
    {
        const void* Indicator = (const void*)1;
        int Phase = -999;
        float StartsAt = -12345.f, Start = -12345.f, Finish = -12345.f, LastR = -12345.f, NextR = -12345.f;
    };

    static FSeen Seen{};
    static UFunction* ServerTimeFn = nullptr;
    static bool bLooked = false;
    static int32 PhaseOffset = -2, StartsAtOffset = -2, IndicatorOffset = -2;
    static int32 StartOffset = -2, FinishOffset = -2, RadiusOffset = -2, LastROffset = -2, NextROffset = -2;

    auto World = UWorld::GetWorld();
    if (!World || !World->GameState)
        return;

    auto GameState = (UObject*)World->GameState;

    static const void* LookedClass = nullptr;

    if (!bLooked || (const void*)GameState->Class != LookedClass)
    {
        bLooked = true;
        LookedClass = (const void*)GameState->Class;
        ServerTimeFn = GameState->GetFunction("GetServerWorldTimeSeconds");
        PhaseOffset = ReflectedOffset(GameState, "SafeZonePhase");
        StartsAtOffset = ReflectedOffset(GameState, "SafeZonesStartTime");
        IndicatorOffset = ReflectedOffset(GameState, "SafeZoneIndicator");
        Seen = FSeen{};
    }

    if (IndicatorOffset < 0)
        return;

    FSeen Now{};
    auto Indicator = GetFromOffset<UObject*>(GameState, (uint32)IndicatorOffset);

    Now.Indicator = (const void*)Indicator;
    Now.Phase = PhaseOffset >= 0 ? (int)GetFromOffset<uint8>(GameState, (uint32)PhaseOffset) : -1;
    Now.StartsAt = StartsAtOffset >= 0 ? GetFromOffset<float>(GameState, (uint32)StartsAtOffset) : -1.f;

    float Radius = -1.f;

    if (Indicator)
    {
        if (StartOffset == -2)
        {
            StartOffset = ReflectedOffset(Indicator, "SafeZoneStartShrinkTime");
            FinishOffset = ReflectedOffset(Indicator, "SafeZoneFinishShrinkTime");
            RadiusOffset = ReflectedOffset(Indicator, "Radius");
            LastROffset = ReflectedOffset(Indicator, "LastRadius");
            NextROffset = ReflectedOffset(Indicator, "NextRadius");
        }

        Now.Start = StartOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)StartOffset) : -1.f;
        Now.Finish = FinishOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)FinishOffset) : -1.f;
        Now.LastR = LastROffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)LastROffset) : -1.f;
        Now.NextR = NextROffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)NextROffset) : -1.f;
        Radius = RadiusOffset >= 0 ? GetFromOffset<float>(Indicator, (uint32)RadiusOffset) : -1.f;
    }

    auto Moved = [](float A, float B, float Tolerance) { return fabsf(A - B) > Tolerance; };

    if (Now.Indicator == Seen.Indicator && Now.Phase == Seen.Phase && !Moved(Now.StartsAt, Seen.StartsAt, 0.25f) &&
        !Moved(Now.Start, Seen.Start, 0.25f) && !Moved(Now.Finish, Seen.Finish, 0.25f) && !Moved(Now.LastR, Seen.LastR, 1.f) &&
        !Moved(Now.NextR, Seen.NextR, 1.f))
        return;

    Seen = Now;

    const float ServerNow = ServerTimeFn ? GameState->Call<float>(ServerTimeFn) : -1.f;

    auto Secs = [](float Value) { return OneDecimal((double)Value); };

    if (!Indicator)
    {
        TellServer(L"storm timer on this screen: no storm yet | phase " + std::to_wstring(Now.Phase) + L" | storm start time " +
                   Secs(Now.StartsAt) + L" (server time now " + Secs(ServerNow) + L", " +
                   (Now.StartsAt >= ServerNow ? L"in " + Secs(Now.StartsAt - ServerNow) : Secs(ServerNow - Now.StartsAt) + L" ago") + L"s)");
        return;
    }

    const wchar_t* What = ServerNow < Now.Start ? L"holding" : (ServerNow < Now.Finish ? L"CLOSING" : L"done, due to move on");

    TellServer(L"storm timer on this screen: phase " + std::to_wstring(Now.Phase) + L", " + What + L" | starts closing in " +
               Secs(Now.Start - ServerNow) + L"s, closed in " + Secs(Now.Finish - ServerNow) + L"s | radius " + Secs(Radius) +
               L", " + Secs(Now.LastR) + L" -> " + Secs(Now.NextR) + L" | server time " + Secs(ServerNow));
}

static bool GGliderReportBroken = false;

static int ReadBoolByName(const UObject* Object, const char* Name)
{
    if (!Object)
        return -1;

    auto Prop = Object->GetProperty(Name, 0x20000);   
    if (!Prop)
        return -1;

    const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
    const uint8 Mask = Prop->GetFieldMask();

    return (GetFromOffset<uint8>(Object, Offset) & (Mask ? Mask : (uint8)0xFF)) ? 1 : 0;
}

static std::wstring WideObjectName(const UObject* Object)
{
    if (!Object)
        return std::wstring(L"NONE");

    const auto Narrow = Object->Name.ToString();
    return std::wstring(Narrow.begin(), Narrow.end());
}

static std::wstring GliderMeshOnScreen(const UObject* Pawn, const char* ComponentName, const char* MeshName, bool* bShowing)
{
    const int32 Offset = ReflectedOffset(Pawn, ComponentName);
    if (Offset < 0)
        return std::wstring(L"-");

    auto Component = GetFromOffset<UObject*>(Pawn, (uint32)Offset);
    if (!Component)
        return std::wstring(L"no component");

    const int32 MeshOffset = ReflectedOffset(Component, MeshName);
    auto Mesh = MeshOffset >= 0 ? GetFromOffset<UObject*>(Component, (uint32)MeshOffset) : nullptr;

    const int Visible = ReadBoolByName(Component, "bVisible");
    const int Hidden = ReadBoolByName(Component, "bHiddenInGame");

    if (Mesh && Visible == 1 && Hidden != 1)
        *bShowing = true;

    return WideObjectName(Mesh) + (Visible == 1 ? (Hidden == 1 ? L" (hidden in game)" : L" (showing)") : L" (not showing)");
}

static void ReportGliders(ULONGLONG Now)
{
    struct FSeen
    {
        float FirstZ = 0.f;
        ULONGLONG FirstAt = 0;
        bool bChecked = false;
    };

    static const UClass* PawnClass = nullptr;
    static bool bDone = false;
    static ULONGLONG FirstScan = 0, LastOpenAt = 0;
    static std::vector<std::pair<const void*, FSeen>> Seen;
    static int Players = 0, Checked = 0, WithGlider = 0, WithMesh = 0, Lines = 0;
    static bool bYouChecked = false;
    static int32 LoadoutOffset = -2, GliderOffset = -2;

    if (bDone)
        return;

    if (FirstScan == 0)
        FirstScan = Now;

    if (!PawnClass)
    {
        PawnClass = FindClass("FortPlayerPawnAthena");

        if (!PawnClass)
        {
            bDone = true;
            TellServer(L"gliders on this screen: this build has no FortPlayerPawnAthena -- not watched.");
            return;
        }
    }

    if (Players == 0 && Now - FirstScan > 600000)
    {
        bDone = true;
        TellServer(L"gliders on this screen: nobody seen under an open glider in 10 minutes -- stopped looking.");
        return;
    }

    auto Local = LocalController();
    const int32 LocalStateOffset = ReflectedOffset(Local, "PlayerState");
    const UObject* LocalState = LocalStateOffset >= 0 ? GetFromOffset<UObject*>(Local, (uint32)LocalStateOffset) : nullptr;

    bool bAnyOpen = false;

    for (int i = 0; i < TUObjectArray::Num(); i++)
    {
        auto Item = TUObjectArray::GetItemByIndex(i);
        if (!Item || !Item->Object || (Item->Flags & (0x20000000 | 0x10000000)))
            continue;

        const UObject* Pawn = Item->Object;
        if (Pawn->IsDefaultObject() || !Pawn->IsA(PawnClass))
            continue;

        if (ReadBoolByName(Pawn, "bIsParachuteOpen") != 1)
            continue;

        bAnyOpen = true;
        LastOpenAt = Now;

        const float Z = (float)((AActor*)Pawn)->K2_GetActorLocation().Z;

        FSeen* Mine = nullptr;
        for (auto& Entry : Seen)
            if (Entry.first == (const void*)Pawn)
                Mine = &Entry.second;

        if (!Mine)
        {
            FSeen First{};
            First.FirstZ = Z;
            First.FirstAt = Now;
            Seen.push_back({ (const void*)Pawn, First });
            Players++;
            continue;
        }

        if (Mine->bChecked || Now - Mine->FirstAt < 3000)
            continue;

        Mine->bChecked = true;
        Checked++;

        if (LoadoutOffset == -2)
        {
            LoadoutOffset = ReflectedOffset(Pawn, "CustomizationLoadout");
            auto Struct = FindStruct("FortAthenaLoadout");
            const uint32 Off = Struct ? Struct->GetOffset("Glider") : (uint32)-1;
            GliderOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;
        }

        const UObject* Glider = (LoadoutOffset >= 0 && GliderOffset >= 0) ? GetFromOffset<UObject*>(Pawn, (uint32)(LoadoutOffset + GliderOffset))
                                                                           : nullptr;
        bool bShowing = false;
        const std::wstring Skeletal = GliderMeshOnScreen(Pawn, "MeshParachuteOpen", "SkeletalMesh", &bShowing);
        const std::wstring Static = GliderMeshOnScreen(Pawn, "ParachuteMeshOpen", "StaticMesh", &bShowing);

        if (Glider)
            WithGlider++;
        if (bShowing)
            WithMesh++;

        const int32 StateOffset = ReflectedOffset(Pawn, "PlayerState");
        const UObject* State = StateOffset >= 0 ? GetFromOffset<UObject*>(Pawn, (uint32)StateOffset) : nullptr;
        const bool bYou = State && State == LocalState;

        if (bYou)
            bYouChecked = true;

        if (!bYou && ++Lines > 8)
            continue;

        const std::wstring Name = PlayerNameOf(State);
        const double Seconds = (double)(Now - Mine->FirstAt) / 1000.0;

        TellServer(L"glider on this screen: '" + (Name.empty() ? std::wstring(L"?") : Name) + L"'" +
                   std::wstring(bYou ? L" (you)" : L"") + L" -- glider in their loadout: " + WideObjectName(Glider) +
                   L" | glider mesh: " + Skeletal + L", umbrella/static mesh: " + Static + L" | still marked skydiving: " +
                   (ReadBoolByName(Pawn, "bIsSkydiving") == 1 ? L"yes" : L"no") + L" | came down " +
                   OneDecimal((double)(Mine->FirstZ - Z) / (Seconds > 0.0 ? Seconds : 1.0)) + L" a second in the " + OneDecimal(Seconds) +
                   L"s after it opened");
    }

    if (!bAnyOpen && Players > 0 && ((bYouChecked && Now - LastOpenAt >= 20000) || Now - LastOpenAt >= 120000))
    {
        bDone = true;
        TellServer(L"gliders on this screen, the drop is over: " + std::to_wstring(Players) + L" player(s) seen under an open glider | " +
                   std::to_wstring(Checked) + L" checked 3s after it opened: " + std::to_wstring(WithGlider) + L" with a glider in their loadout, " +
                   std::to_wstring(WithMesh) + L" with a glider mesh showing.");
    }
}

static int ReportGlidersGuarded(ULONGLONG Now)
{
    __try
    {
        ReportGliders(Now);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static void BeforeEvent(UObject* Object, UFunction* Function, void* Params)
{
    const int32 Name = Function->Name.ComparisonIndex;

    if (Name == GNameOnRepDeathInfo.ComparisonIndex)
    {
        if (!GKillFeedBroken)
            OnDeathInfoArrived(Object);
    }
    else if (Name == GNameUpdateKillFeed.ComparisonIndex)
    {
        
        GKillFeedWidget = Object;
        GKillFeedWidgetIndex = Object ? Object->Index : -1;

        if (!GKillFeedBroken)
            LookAtTheKillFeed();
    }
    else if (Name == GNameClientReportKill.ComparisonIndex)
    {
        OnClientReportKill(Function, Params);
    }
    else if (Name == GNameServerCreateBuildingActor.ComparisonIndex && Params)
    {
        
        if (GCreateUsable && GCreateDataOffset >= 0)
        {
            GTurbo.PreviousLevel = *(int32*)((uint8*)Params + GCreateDataOffset + 8);
            GTurbo.UpgradeLevel = *(int32*)((uint8*)Params + GCreateDataOffset + 12);
        }
    }
}

static void GameThreadTick()
{
    const ULONGLONG Now = GetTickCount64();

    static ULONGLONG LastTurbo = 0, LastKillFeed = 0, LastStorm = 0, LastGliders = 0;

    if (GFaultToReport)
    {
        GFaultToReport = false;
        TellServer(L"the client DLL's round-4 code hit a fault -- the killfeed lines and F7 turbo building are switched off for the "
                   L"rest of this game" + std::wstring(GStormReportBroken ? L" (and these reports too)" : L"") + L".");
    }

    if (const int Toggles = GTurboToggles.exchange(0))
    {
        (void)Toggles;
    }

    if (Now - LastTurbo >= 16)
    {
        LastTurbo = Now;
        TurboBuildTick(Now);
    }

    if (!GKillFeedBroken && !GNoKillerDeaths.empty() && Now - LastKillFeed >= 250)
    {
        LastKillFeed = Now;

        if (LookAtTheKillFeed())
            RefreshKillFeedWidget();
    }

    if (!GStormReportBroken && Now - LastStorm >= 1000)
    {
        LastStorm = Now;
        ReportStorm();
    }

    if (!GGliderReportBroken && !GReportsBroken && Now - LastGliders >= 1000)
    {
        LastGliders = Now;

        if (ReportGlidersGuarded(Now))
        {
            GGliderReportBroken = true;
            TellServer(L"gliders on this screen: the client DLL's glider check hit a fault -- that check is switched off (nothing else is).");
        }
    }
}

static int BeforeEventGuarded(UObject* Object, UFunction* Function, void* Params)
{
    __try
    {
        BeforeEvent(Object, Function, Params);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static int GameThreadTickGuarded()
{
    __try
    {
        GameThreadTick();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}

static void ProcessEventHook(UObject* Object, UFunction* Function, void* Params)
{
    const DWORD GameThread = GGameThreadId.load(std::memory_order_relaxed);

    if (GInOurCode || GFaults >= 3 || GameThread == 0 || !Function || GetCurrentThreadId() != GameThread)
        return ProcessEventOG(Object, Function, Params);

    GInOurCode++;
    const int BeforeFault = BeforeEventGuarded(Object, Function, Params);
    GInOurCode--;

    ProcessEventOG(Object, Function, Params);

    GInOurCode++;
    const int TickFault = GameThreadTickGuarded();
    GInOurCode--;

    if (BeforeFault || TickFault)
    {

        GFaults++;
        GTurboBroken = true;
        GKillFeedBroken = true;

        if (GFaults == 1)
        {
            GFaultToReport = true;
        }
        else
        {
            GReportsBroken = true;
            GStormReportBroken = true;
            GFaultToReport = false;
        }
    }
}

static HWND GGameWindow = nullptr;
static WNDPROC GGameWindowProcOG = nullptr;
static UINT GInstallHookMessage = 0;
static std::atomic<bool> GHookInstalled{ false };

static void InstallProcessEventHookNow()
{
    auto DefaultObject = UObject::StaticClass() ? UObject::StaticClass()->GetDefaultObj() : nullptr;
    void* ProcessEventAddress = DefaultObject ? DefaultObject->Vft[Offsets::ProcessEventVft] : nullptr;

    if (ProcessEventAddress)
        Hooking::Hook(ProcessEventAddress, (void*)ProcessEventHook, ProcessEventOG);
}

static LRESULT CALLBACK GameWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    if (GInstallHookMessage != 0 && Message == GInstallHookMessage)
    {
        if (!GHookInstalled.exchange(true))
            InstallProcessEventHookNow();

        return 0;
    }

    return CallWindowProcW(GGameWindowProcOG, Window, Message, WParam, LParam);
}

static void AskTheGameThreadForTheHook()
{
    if (GHookInstalled.load())
        return;

    if (!GGameWindow)
    {
        FGameWindow Found{};
        EnumWindows(FindGameWindow, (LPARAM)&Found);

        if (!Found.Window || !Found.Thread)
            return;

        GInstallHookMessage = RegisterWindowMessageW(L"Four_E_Zero_H 1.8.2 client: install the game-thread hook");

        if (!GInstallHookMessage)
            return;

        GGameWindowProcOG = (WNDPROC)GetWindowLongPtrW(Found.Window, GWLP_WNDPROC);

        if (!GGameWindowProcOG)
            return;

        SetWindowLongPtrW(Found.Window, GWLP_WNDPROC, (LONG_PTR)GameWindowProc);

        GGameWindow = Found.Window;
        GGameThreadId.store(Found.Thread);
    }

    PostMessageW(GGameWindow, GInstallHookMessage, 0, 0);
}

static void InstallGameThreadHook()
{
    GNameOnRepDeathInfo = FName(L"OnRep_DeathInfo");
    GNameUpdateKillFeed = FName(L"UpdateKillFeed");
    GNameClientReportKill = FName(L"ClientReportKill");
    GNameServerCreateBuildingActor = FName(L"ServerCreateBuildingActor");

}

void ClientThread()
{
    bool bPressed = false;

    DWORD LastJoinTick = 0;

    while (true)
    {
        if (UWorld::GetWorld() && UWorld::GetWorld()->OwningGameInstance)
        {
            auto& LocalPlayers = UWorld::GetWorld()->OwningGameInstance->LocalPlayers;

            if (LocalPlayers.Num() > 0)
            {
                auto PlayerController = (AFortPlayerControllerAthena*)LocalPlayers[0]->PlayerController;

                if (PlayerController && !PlayerController->CheatManager)
                {
                    PlayerController->CheatManager = (UFortCheatManager*)UGameplayStatics::SpawnObject(PlayerController->CheatClass.Get(), PlayerController);
                    PlayerController->CheatManager->ObjectFlags &= ~0x1000000;
                    TUObjectArray::GetItemByIndex(PlayerController->CheatManager->Index)->Flags &= ~0x4000000;
                }
            }
        }

        AskTheGameThreadForTheHook();

        if (!bPressed && GetAsyncKeyState(VK_F3))
        {
            bPressed = true;

            bEOREnabled ^= 1;
        }
        else if (!bPressed && GetAsyncKeyState(VK_F4))
        {
            bPressed = true;

            bDisablePreEdits ^= 1;
        }
        else if (!bPressed && GetAsyncKeyState(VK_F5))
        {
            bPressed = true;

            const DWORD Now = GetTickCount();

            if (LastJoinTick == 0 || Now - LastJoinTick > 3000)
            {
                LastJoinTick = Now;
                JoinLocalServer();
            }
        }

        else if (!bPressed && GetAsyncKeyState(VK_F7))
        {
            bPressed = true;

            const bool bNowOn = !GTurboBuild.load();
            GTurboBuild.store(bNowOn);
            GTurboToggles++;
        }

        else if (!GetAsyncKeyState(VK_F3) && !GetAsyncKeyState(VK_F4) && !GetAsyncKeyState(VK_F5) && !GetAsyncKeyState(VK_F7))
            bPressed = false;

        Sleep(33);
    }
}

void Client::Init()
{
    UEngine::GetEngine()->GameViewport->ViewportConsole = UGameplayStatics::SpawnObject(UEngine::GetEngine()->ConsoleClass, UEngine::GetEngine()->GameViewport);

    if (VersionInfo.FortniteVersion >= 10 || FConfiguration::bForceRespawns)
    {
        auto PrimarySlot = uint8_t(EPlaylistUIExtensionSlot::StaticEnum() ? EPlaylistUIExtensionSlot::GetPrimary() : EUIExtensionSlot::GetPrimary());

        TArray<FUIExtension> ArenaExtensions, ShowdownExtensions;

        FUIExtension ArenaUIExtension{};
        ArenaUIExtension.Slot = PrimarySlot;
        if (VersionInfo.FortniteVersion < 23)
            ArenaUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD.ArenaScoringHUD_C");
        else
        {
            auto& PackageName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));
            auto& AssetName = *(FName*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0xC));
            auto& SubPathString = *(FString*)(__int64(&ArenaUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));

            PackageName = FName(L"/Game/UI/Competitive/Arena/ArenaScoringHUD");
            AssetName = FName(L"ArenaScoringHUD_C");
            SubPathString = FString();
        }

        FUIExtension ShowdownUIExtension{};
        ShowdownUIExtension.Slot = PrimarySlot;
        if (VersionInfo.FortniteVersion < 23)
            ShowdownUIExtension.WidgetClass.ObjectID.AssetPathName = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD.ShowdownScoringHUD_C");
        else
        {
            auto& PackageName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));
            auto& AssetName = *(FName*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0xC));
            auto& SubPathString = *(FString*)(__int64(&ShowdownUIExtension.WidgetClass) + (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));

            PackageName.ComparisonIndex = FName(L"/Game/UI/Frontend/Showdown/ShowdownScoringHUD").ComparisonIndex;
            AssetName.ComparisonIndex = FName(L"ShowdownScoringHUD_C").ComparisonIndex;
            SubPathString = FString();
        }

        auto PlaylistClass = FindClass("FortPlaylistAthena");

        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            auto Object = TUObjectArray::GetObjectByIndex(i);

            if (Object && Object->IsA((UClass*)PlaylistClass))
            {
                auto Playlist = (UFortPlaylistAthena*)Object;

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
                    if (Playlist->HasbAllowJoinInProgress())
                        Playlist->bAllowJoinInProgress = true;

                    if (Playlist->HasbForceCameraFadeOnRespawn())
                        Playlist->bForceCameraFadeOnRespawn = true;
                }
                if (VersionInfo.FortniteVersion >= 10)
                {
                    auto Name = Object->Name.ToString();
                    if (Name.contains("Showdown"))
                        Playlist->UIExtensions.Add(Name.contains("ShowdownAlt") ? ArenaUIExtension : ShowdownUIExtension);
                }
            }
        }
    }

    if (VersionInfo.FortniteVersion < 24.30)
    {
        auto EditModeInputComponent = Memcury::Scanner::FindStringRef<const wchar_t*>(L"EditModeInputComponent0");

        auto rdataSect = Memcury::PE::Section::GetSection(".rdata");
        for (int i = 1; i < 0x5000; i++)
        {
            auto Addr = (uint8_t*)EditModeInputComponent.Get() + i;

            if (*Addr == 0x48 && *(Addr + 1) == 0x8D)
            {
                auto Rel = Memcury::Scanner(Addr).RelativeOffset(3);

                if (rdataSect.isInSection(Rel.Get()) && strcmp((char*)Rel.Get(), "CompleteBuildingEditInteraction") == 0)
                {
                    CompleteBuildingEditInteraction = Memcury::Scanner(Addr).ScanFor({ 0x48, 0x8D }, false).RelativeOffset(3).GetAs<void (*)(void*)>();
                    break;
                }
            }
        }

        auto SelectEditScanner = EditModeInputComponent;
        SelectEditScanner.ScanFor({ 0x48, 0x8D, 0x05 }, true, 1);

        if (VersionInfo.FortniteVersion < 24.30)
        {
            auto SelectResetScanner = SelectEditScanner;
            auto SelectResetAddr = SelectResetScanner.ScanFor({ 0x48, 0x8D, 0x05 }, true).RelativeOffset(3).Get();

            Hooking::Hook(SelectResetAddr, SelectReset, SelectResetOG);
        }

        if (VersionInfo.FortniteVersion < 11)
        {
            auto SelectEditAddr = SelectEditScanner.RelativeOffset(3).Get();

            Hooking::Hook(SelectEditAddr, SelectEdit, SelectEditOG);
        }

        if (VersionInfo.FortniteVersion < 15.20)
        {
            auto rdataSect = Memcury::PE::Section::GetSection(".rdata");
            for (int i = 1; i < 0x5000; i++)
            {
                auto Addr = (uint8_t*)EditModeInputComponent.Get() - i;

                if (*Addr == 0x48 && *(Addr + 1) == 0x8D)
                {
                    auto Rel = Memcury::Scanner(Addr).RelativeOffset(3);

                    if (rdataSect.isInSection(Rel.Get()) && strcmp((char*)Rel.Get(), "PerformBuildingEditInteraction") == 0)
                    {
                        auto PerformBuildingEditInteractionAddr = Memcury::Scanner(Addr).ScanFor({ 0x48, 0x8D }, false).RelativeOffset(3).Get();

                        Hooking::Hook(PerformBuildingEditInteractionAddr, PerformBuildingEditInteraction, PerformBuildingEditInteractionOG);
                        break;
                    }
                }
            }
        }
    }

    InstallGameThreadHook();

    CreateThread(0, 0, (LPTHREAD_START_ROUTINE)ClientThread, 0, 0, 0);
}
