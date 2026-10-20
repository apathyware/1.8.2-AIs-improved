#include "pch.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../../Four_E_Zero_H/Public/BotAI.h"
#include "../../Four_E_Zero_H/Public/Configuration.h"
#include "../../Four_E_Zero_H/Public/GUI.h"

uint64_t SetGamePhase_ = 0;
void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetGamePhase(EAthenaGamePhase GamePhase)
{
    auto SetGamePhaseInternal = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, EAthenaGamePhase))SetGamePhase_;

    if (SetGamePhaseInternal)
        return SetGamePhaseInternal(this, GamePhase);
    else
    {
        static auto GamePhaseOffset = this->GetOffset("GamePhase");
        auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

        auto OldGamePhase = _GamePhase;
        _GamePhase = GamePhase;
        OnRep_GamePhase(OldGamePhase);
    }
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetGamePhaseStep(EAthenaGamePhaseStep GamePhaseStep)
{
    static auto GamePhaseStepOffset = this->GetOffset("GamePhaseStep");
    auto& _GamePhaseStep = *(EAthenaGamePhaseStep*)(__int64(this) + GamePhaseStepOffset);

    _GamePhaseStep = GamePhaseStep;
    HandleGamePhaseStepChanged(GamePhaseStep);
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::HandleMatchHasStarted(AFortGameMode* GameMode)
{
    HandleMatchHasStartedOG(GameMode);
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameMode);

    if (!bSkipWarmup)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        auto WarmupDuration = 60.f;

        GamePhaseLogic->WarmupCountdownStartTime = Time;
        GamePhaseLogic->WarmupCountdownEndTime = Time + WarmupDuration;
        GamePhaseLogic->WarmupCountdownDuration = 10.f;
        GamePhaseLogic->WarmupEarlyCountdownDuration = WarmupDuration - 10.f;

        GamePhaseLogic->SetGamePhase(EAthenaGamePhase::Warmup);
        GamePhaseLogic->SetGamePhaseStep(EAthenaGamePhaseStep::Warmup);
    }
    else
    {
        printf("[GamePhaseLogic] Skipping warmup\n");
        GamePhaseLogic->StartAircraftPhase();
    }
}

AFortSafeZoneIndicator* UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetupSafeZoneIndicator()
{
    if (!this->SafeZoneIndicator)
    {
        AFortSafeZoneIndicator* SafeZoneIndicator = UWorld::SpawnActor<AFortSafeZoneIndicator>(SafeZoneIndicatorClass, FVector{});

        if (SafeZoneIndicator)
        {
            auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
            FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
            float SafeZoneCount = SafeZoneDefinition.Count.Evaluate();

            auto& Array = SafeZoneIndicator->SafeZonePhases;

            if (Array.IsValid())
                Array.Free();

            const float Time = (float)UGameplayStatics::GetTimeSeconds(GameState);

            for (float i = 0; i < SafeZoneCount; i++)
            {
                auto PhaseInfo = (FFortSafeZonePhaseInfo*)malloc(FFortSafeZonePhaseInfo::Size());
                memset((PBYTE)PhaseInfo, 0, FFortSafeZonePhaseInfo::Size());

                PhaseInfo->Radius = SafeZoneDefinition.Radius.Evaluate(i);
                PhaseInfo->WaitTime = SafeZoneDefinition.WaitTime.Evaluate(i);
                PhaseInfo->ShrinkTime = SafeZoneDefinition.ShrinkTime.Evaluate(i);
                PhaseInfo->PlayerCap = (int)SafeZoneDefinition.PlayerCapSolo.Evaluate(i);

                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->AthenaGameDataTable, FName(L"Default.SafeZone.Damage"), i, nullptr, &PhaseInfo->DamageInfo.Damage, FString());

                {
                    static const float StormDamagePerTick[] = { 1.f, 1.f, 2.f, 5.f, 8.f, 10.f, 10.f, 10.f, 10.f, 10.f };
                    const int Phases = (int)(sizeof(StormDamagePerTick) / sizeof(StormDamagePerTick[0]));

                    int Which = (int)i;
                    if (Which < 0)
                        Which = 0;
                    if (Which >= Phases)
                        Which = Phases - 1;

                    float Damage = StormDamagePerTick[Which] * FConfiguration::StormDamageScale;
                    if (!(Damage > 0.f))
                        Damage = 0.f;

                    PhaseInfo->DamageInfo.Damage = Damage;
                    PhaseInfo->DamageInfo.bPercentageBasedDamage = false;

                    printf("[SafeZone] Phase %d storm damage set to %.1f a tick (flat, so it eats shield first).\n", Which, Damage);
                }
                PhaseInfo->TimeBetweenStormCapDamage = TimeBetweenStormCapDamage.Evaluate(i);
                PhaseInfo->StormCapDamagePerTick = StormCapDamagePerTick.Evaluate(i);
                PhaseInfo->StormCampingIncrementTimeAfterDelay = StormCampingIncrementTimeAfterDelay.Evaluate(i);
                PhaseInfo->StormCampingInitialDelayTime = StormCampingInitialDelayTime.Evaluate(i);
                PhaseInfo->MegaStormGridCellThickness = (int)SafeZoneDefinition.MegaStormGridCellThickness.Evaluate(i);

                if (FFortSafeZonePhaseInfo::HasUsePOIStormCenter())
                    PhaseInfo->UsePOIStormCenter = false;

                PhaseInfo->Center = SafeZoneLocations[(int)i];

                Array.Add(*PhaseInfo, FFortSafeZonePhaseInfo::Size());
                free(PhaseInfo);

                SafeZoneIndicator->PhaseCount++;
            }

            SafeZoneIndicator->OnRep_PhaseCount();

            if (FConfiguration::bDisableStorm)
            {
                SafeZoneIndicator->SafeZoneStartShrinkTime = Time + 999999999.f;
                SafeZoneIndicator->SafeZoneFinishShrinkTime = Time + 999999999.f;
            }
            else
            {
                SafeZoneIndicator->SafeZoneStartShrinkTime = Time + Array[0].WaitTime;
                SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + Array[0].ShrinkTime;
            }

            SafeZoneIndicator->CurrentPhase = 0;
            SafeZoneIndicator->OnRep_CurrentPhase();
        }

        this->SafeZoneIndicator = SafeZoneIndicator;
        OnRep_SafeZoneIndicator();
    }

    return this->SafeZoneIndicator;
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::StartNewSafeZonePhase(int NewSafeZonePhase, bool bInitial)
{
    if (FConfiguration::bDisableStorm)
    {
        if (SafeZoneIndicator)
        {
            float NowSec = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
            SafeZoneIndicator->SafeZoneStartShrinkTime = NowSec + 999999999.f;
            SafeZoneIndicator->SafeZoneFinishShrinkTime = NowSec + 999999999.f;
            SafeZoneIndicator->CurrentPhase = 0;
        }
        return;
    }

    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    auto& Array = SafeZoneIndicator->SafeZonePhases;

    if (Array.IsValidIndex(NewSafeZonePhase))
    {
        if (Array.IsValidIndex(NewSafeZonePhase - 1))
        {
            auto& PreviousPhaseInfo = Array.Get(NewSafeZonePhase - 1, FFortSafeZonePhaseInfo::Size());

            SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
            SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
        }

        auto& PhaseInfo = Array.Get(NewSafeZonePhase, FFortSafeZonePhaseInfo::Size());

        SafeZoneIndicator->NextCenter = PhaseInfo.Center;
        SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
        SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

        if (Array.IsValidIndex(NewSafeZonePhase + 1))
        {
            auto& NextPhaseInfo = Array.Get(NewSafeZonePhase + 1, FFortSafeZonePhaseInfo::Size());

            if (SafeZoneIndicator->FutureReplicator)
            {
                SafeZoneIndicator->FutureReplicator->NextNextCenter = NextPhaseInfo.Center;
                SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhaseInfo.Radius;
            }

            SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
            SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
            SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
        }

        SafeZoneIndicator->SafeZoneStartShrinkTime = FConfiguration::bLateGame && FConfiguration::bLateGameLongZone ? 676767.f : TimeSeconds + PhaseInfo.WaitTime;
        SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseInfo.ShrinkTime;

        SafeZoneIndicator->CurrentDamageInfo = PhaseInfo.DamageInfo;
        SafeZoneIndicator->OnRep_CurrentDamageInfo();

        auto OldPhase = SafeZoneIndicator->CurrentPhase;
        SafeZoneIndicator->CurrentPhase = NewSafeZonePhase;
        SafeZoneIndicator->OnRep_CurrentPhase();

        SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

        auto& SafeZoneState = *(uint8_t*)(__int64(&SafeZoneIndicator->FutureReplicator) - 0x4);
        SafeZoneState = 2;
        bool bInitial = OldPhase <= 0;

        SafeZoneIndicator->OnSafeZoneStateChange(2, bInitial);
        SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, 2);

        SetGamePhaseStep(EAthenaGamePhaseStep::StormHolding);

        auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
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

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::StartAircraftPhase()
{
    static auto GamePhaseOffset = this->GetOffset("GamePhase");
    auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

    if (_GamePhase >= EAthenaGamePhase::Aircraft)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
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

    if (FConfiguration::bJoinInProgress || (Playlist && Playlist->bAllowJoinInProgress))
        *(bool*)(uint64_t(&GameMode->WarmupRequiredPlayerCount) - 4) = false;

    if (bSkipAircraft)
    {
        printf("[GamePhaseLogic] Skipping aircraft\n");
        SetGamePhase(EAthenaGamePhase::SafeZones);
        SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);

        return;
    }

    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;

    if (GameState->MapInfo->FlightInfos.Num() > 0)
    {
        auto& FlightInfo = GameState->MapInfo->FlightInfos[0];

        if (FConfiguration::bLateGame)
        {
            GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;

            FVector Loc = SafeZoneLocations[FConfiguration::LateGameZone + 2];
            Loc.Z = 17500.f;

            FlightInfo.FlightSpeed = 0.f;

            FlightInfo.FlightStartLocation = Loc;

            FlightInfo.TimeTillFlightEnd = 7.f;
            FlightInfo.TimeTillDropEnd = 7.f;
            FlightInfo.TimeTillDropStart = 0.f;
        }

        if (!GameState->MapInfo->AircraftClass.Get())
            return;
        auto Aircraft = AFortAthenaAircraft::SpawnAircraft(UWorld::GetWorld(), GameState->MapInfo->AircraftClass, FlightInfo);

        if (!Aircraft)
            return;

        Aircraft->FlightElapsedTime = 0;
        Aircraft->DropStartTime = (float)Time + FlightInfo.TimeTillDropStart;
        Aircraft->DropEndTime = (float)Time + FlightInfo.TimeTillDropEnd;
        Aircraft->FlightStartTime = (float)Time;
        Aircraft->FlightEndTime = (float)Time + FlightInfo.TimeTillFlightEnd;
        Aircraft->ReplicatedFlightTimestamp = (float)Time;
        bAircraftIsLocked = true;

        for (auto& Player__Uncasted : ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers)
        {
            auto Player = (AFortPlayerControllerAthena*)Player__Uncasted;
            auto Pawn = (AFortPlayerPawnAthena*)Player->Pawn;

            if (Pawn)
            {
                if (Pawn->Role == 3)
                {
                    if (Pawn->bIsInAnyStorm)
                    {
                        Pawn->bIsInAnyStorm = false;
                        Pawn->OnRep_IsInAnyStorm();
                    }
                }
                Pawn->bIsInsideSafeZone = true;
                Pawn->OnRep_IsInsideSafeZone();
                Pawn->OnEnteredAircraft.Process();
            }

            Player->ClientActivateSlot(0, 0, 0.f, true, true);
            if (Pawn)
                Pawn->K2_DestroyActor();
            auto Reset = (void (*)(AFortPlayerControllerAthena*))FindReset();
            Reset(Player);
            Player->ClientGotoState(FName(L"Spectating"));
        }

        Aircrafts_GameMode.Add(Aircraft);
        Aircrafts_GameState.Add(Aircraft);
    }

    SetGamePhase(EAthenaGamePhase::Aircraft);
    SetGamePhaseStep(EAthenaGamePhaseStep::BusLocked);
}

uint64_t Reset_ = 0;
uint64_t IsInsideSafeZone = 0;
void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Tick()
{
    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    static auto GamePhaseOffset = this->GetOffset("GamePhase");
    auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

    static bool finishedFlight = false;
    if (!bSkipAircraft)
    {
        if (_GamePhase <= EAthenaGamePhase::Warmup)
        {
            static bool gettingReady = false;
            static bool bWasHeld = false;

            if (!bStartAircraft && !BotAI::BusMayLaunch())
            {
                bWasHeld = true;
                gettingReady = false;

                float HeldOpen = (float)Time + 600.f;

                WarmupCountdownStartTime = (float)Time;
                WarmupCountdownEndTime = HeldOpen;
                WarmupEarlyCountdownDuration = HeldOpen;

                if (auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState)
                {
                    if (GameState->HasWarmupCountdownStartTime())
                        GameState->WarmupCountdownStartTime = (float)Time;
                    if (GameState->HasWarmupCountdownEndTime())
                        GameState->WarmupCountdownEndTime = HeldOpen;
                }

                if (auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)
                {
                    if (GameMode->HasWarmupEarlyCountdownDuration())
                        GameMode->WarmupEarlyCountdownDuration = HeldOpen;
                }

                static bool bSaidHeld = false;
                if (!bSaidHeld)
                {
                    bSaidHeld = true;
                    printf("[Warmup] Holding the bus: it leaves when the lobby reaches %d, and not before.\n",
                           FConfiguration::AutoFillTargetPlayerCount);
                    fflush(stdout);
                }

                return;
            }

            if (bWasHeld)
            {
                bWasHeld = false;
                gettingReady = false;

                float LaunchIn = 10.f;
                float StartsNow = (float)Time;
                float LeavesAt = (float)Time + LaunchIn;

                WarmupCountdownStartTime = StartsNow;
                WarmupCountdownEndTime = LeavesAt;
                WarmupCountdownDuration = LaunchIn;
                WarmupEarlyCountdownDuration = StartsNow;

                if (auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState)
                {
                    if (GameState->HasWarmupCountdownStartTime())
                        GameState->WarmupCountdownStartTime = StartsNow;
                    if (GameState->HasWarmupCountdownEndTime())
                        GameState->WarmupCountdownEndTime = LeavesAt;
                }

                if (auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)
                {
                    if (GameMode->HasWarmupEarlyCountdownDuration())
                        GameMode->WarmupEarlyCountdownDuration = StartsNow;
                }

                printf("[Warmup] The lobby is full. The bus leaves in %.0f seconds.\n", LaunchIn);
                fflush(stdout);
            }

            if (!bStartAircraft && !gettingReady)
            {
                if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && WarmupEarlyCountdownDuration != -1 && WarmupEarlyCountdownDuration < Time)
                {
                    gettingReady = true;

                    SetGamePhaseStep(EAthenaGamePhaseStep::GetReady);
                    return;
                }
            }

            if (bStartAircraft || gettingReady)
            {
                if (bStartAircraft || (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && WarmupCountdownEndTime != -1 && WarmupCountdownEndTime < Time))
                {
                    StartAircraftPhase();

                    return;
                }
            }
        }

        if (_GamePhase == EAthenaGamePhase::Aircraft)
        {
            static bool busUnlocked = false;
            if (!busUnlocked)
            {
                if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->DropStartTime < Time)
                {
                    busUnlocked = true;

                    bAircraftIsLocked = false;
                    SetGamePhaseStep(EAthenaGamePhaseStep::BusFlying);
                    return;
                }
            }

            static bool startedForming = false;
            if (!startedForming)
            {
                if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->DropEndTime != -1 &&
                    Aircrafts_GameState[0]->DropEndTime < Time)
                {
                    startedForming = true;
                    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    for (auto& Player__Uncasted : GameMode->AlivePlayers)
                    {
                        auto Player = (AFortPlayerControllerAthena*)Player__Uncasted;
                        if (!Player->PlayerState->bIsABot && Player->IsInAircraft())
                        {
                            Player->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
                        }
                    }

                    if (FConfiguration::bLateGame)
                        SafeZonesStartTime = (float)Time;
                    else
                        SafeZonesStartTime = (float)Time + 60.f;

                    SetGamePhase(EAthenaGamePhase::SafeZones);
                    SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);
                    return;
                }
            }
        }

        if (!finishedFlight)
        {
            if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->FlightEndTime != -1 &&
                Aircrafts_GameState[0]->FlightEndTime < Time)
            {
                finishedFlight = true;
                auto Aircraft = Aircrafts_GameState[0].Get();

                Aircraft->K2_DestroyActor();
                Aircrafts_GameState.Clear();
                Aircrafts_GameMode.Clear();
                return;
            }
        }
    }
    else
    {
        if (!finishedFlight)
        {
            finishedFlight = true;

            SetGamePhase(EAthenaGamePhase::SafeZones);
            SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);
        }
    }

    if (bEnableZones)
    {
        if (_GamePhase == EAthenaGamePhase::SafeZones)
        {
            static bool formedZone = false;
            if (!bPausedZone && finishedFlight && !formedZone)
            {
                if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && SafeZonesStartTime != -1 && SafeZonesStartTime < Time)
                {
                    formedZone = true;
                    auto SafeZoneIndicator = SetupSafeZoneIndicator();
                    StartNewSafeZonePhase(FConfiguration::bLateGame ? FConfiguration::LateGameZone + 3 : 1, true);
                    return;
                }
            }

            static bool bUpdatedPhase = false;
            if (formedZone && SafeZoneIndicator)
            {
                if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase))
                {
                    bool bStartedNewPhase = false;
                    if (!bPausedZone && !bUpdatedPhase && SafeZoneIndicator->SafeZoneStartShrinkTime < Time)
                    {
                        bUpdatedPhase = true;

                        auto& SafeZoneState = *(uint8_t*)(__int64(&SafeZoneIndicator->FutureReplicator) - 0x4);
                        SafeZoneState = 3;

                        SafeZoneIndicator->OnSafeZoneStateChange(3, false);
                        SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, 3);

                        SetGamePhaseStep(EAthenaGamePhaseStep::StormShrinking);
                    }
                    else if (!bPausedZone && SafeZoneIndicator->SafeZoneFinishShrinkTime < Time)
                    {
                        bStartedNewPhase = true;

                        if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase + 1))
                        {
                            StartNewSafeZonePhase(SafeZoneIndicator->CurrentPhase + 1);
                            bUpdatedPhase = false;
                        }
                    }
                }

                auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                static auto ZoneEffect = FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C");

                static int StormBatchCursor = 0;
                const int StormBatchSize = 10;

                int PlayerCount = GameMode->AlivePlayers.Num();
                int Processed = 0;
                while (PlayerCount > 0 && Processed < StormBatchSize)
                {
                    if (StormBatchCursor >= PlayerCount)
                        StormBatchCursor = 0;

                    auto Player = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[StormBatchCursor];
                    StormBatchCursor++;
                    Processed++;

                    if (auto Pawn = Player->MyFortPawn)
                    {
                        bool bInZone = IsInCurrentSafeZone(Player->MyFortPawn->K2_GetActorLocation(), false);

                        if (!IsInCurrentSafeZone__Ptr)
                            bInZone = GameMode->IsInCurrentSafeZone(Player->MyFortPawn->K2_GetActorLocation(), false);

                        if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
                        {
                            printf("Pawn %s new storm status: %s\n", Pawn->Name.ToString().c_str(), bInZone ? "true" : "false");
                            Pawn->bIsInAnyStorm = !bInZone;
                            Pawn->OnRep_IsInAnyStorm();
                            Pawn->bIsInsideSafeZone = bInZone;
                            Pawn->OnRep_IsInsideSafeZone();
                        }
                    }
                }
            }
        }
    }
}

struct FVector4
{
public:
    double X;
    double Y;
    double Z;
    double W;
};

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::InitializeSafeZoneLocations()
{
    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    auto Playlist = (UFortPlaylistAthena*)GameState->CurrentPlaylistInfo.BasePlaylist;

    auto SafeZoneBlacklist = Playlist->SafeZoneLocationBlacklist.Get();

    if (!SafeZoneBlacklist)
        SafeZoneBlacklist = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaSafeZoneBlacklist.AthenaSafeZoneBlacklist");

    auto& SZBCurve = (TMap<FName, FRealCurve*>&)SafeZoneBlacklist->RowMap;

    TArray<FVector4> BlacklistLocations;

    for (auto& [Key, Curve] : SZBCurve)
    {
        FSimpleCurve* Row = (FSimpleCurve*)Curve;

        if (!Row)
            continue;

        FVector4 Loc{};

        for (auto& Key : Row->Keys)
        {
            if (Key.Time == 0.f)
                Loc.X = Key.Value;
            else if (Key.Time == 1.f)
                Loc.Y = Key.Value;
            else if (Key.Time == 2.f)
                Loc.Z = Key.Value;
            else if (Key.Time == 3.f)
                Loc.W = Key.Value;
        }

        if (Loc.X == 0 && Loc.Y == 0 && Loc.Z == 0 && Loc.W == 0)
            continue;

        BlacklistLocations.Add(Loc);
    }

    auto ZeroVector = FVector(0, 0, 0);
    auto SafeZoneCount = (float)Playlist->LastSafeZoneIndex;

    if (SafeZoneCount == -1)
        SafeZoneCount = GameState->MapInfo->SafeZoneDefinition.Count.Evaluate(0.f);
    else
        SafeZoneCount++;

    auto Center = GameState->MapInfo->GetMapCenter();

    SafeZoneLocations.Clear();
    SafeZoneLocations.Reserve((int)SafeZoneCount);

    for (int i = (int)(SafeZoneCount - 1); i >= 0; i--)
    {
        auto Params = GameState->MapInfo->ConstructSafeZoneLocationParams(i, Center, i == SafeZoneCount - 1 ? ZeroVector : SafeZoneLocations[i + 1], i == SafeZoneCount - 1, 0);

        auto Location = GameState->MapInfo->PickSafeZoneLocation(Params, BlacklistLocations);

        SafeZoneLocations[i] = Location;
    }
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Hook()
{
    if (!GetDefaultObj())
        return;

    Reset_ = FindReset();
    SetGamePhase_ = FindSetGamePhase();

    Hooking::Hook(FindHandleMatchHasStarted(), HandleMatchHasStarted, HandleMatchHasStartedOG);
}
