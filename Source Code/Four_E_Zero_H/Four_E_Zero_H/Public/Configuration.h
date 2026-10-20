#pragma once

struct FConfiguration
{
    static inline auto Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    static inline auto MaxTickRate = 30;

    static inline auto bDisableStorm = false;

    static inline auto bUseMapStormTimes = true;

    static inline auto StormPhaseTimeScale = 1.f;

    static inline auto StormStartPhase = 1;

    static inline auto bStormLog = true;

    static inline auto StormLogIntervalSeconds = 5.f;

    static inline auto bBotLog = true;

    static inline auto bManageStormDamage = false;

    static inline auto StormDamageScale = 1.f;

    static inline auto bBotsFleeTheStorm = true;

    static inline auto BotZoneEntryFraction = 0.5f;

    static inline auto bRecordPlaceDump = true;

    static inline auto PlaceDumpHotkey = 0x73;

    static inline auto bUsePlaceDump = true;

    static inline auto PlaceDumpFolder = "C:\\4e0h\\Locations";

    static inline auto PlaceDumpIntervalSeconds = 0.1f;

    static inline auto PlaceDumpMinSpacing = 120.f;

    static inline auto bRecordBots = true;

    static inline auto PlaceDumpBotIntervalSeconds = 1.0f;

    static inline auto PlaceDumpBotMinSpacing = 300.f;

    static inline auto PlaceDumpHeightCeiling = 12000.f;

    static inline auto PlaceDumpMaxStep = 1200.f;

    static inline auto bBotCombatEnabled = true;

    static inline auto BotSprintSpeed = 510.f;

    static inline auto bBotsFaceTheirDirection = true;

    static inline auto BotFacingMinimumSpeed = 40.f;

    static inline auto bBotsNeverRunOutOfAmmo = true;

    static inline auto bDisableStormSurge = true;

    static inline auto bLockServerSpeed = true;

    static inline auto bAllBotsFight = true;

    static inline auto BotEngageRange = 12000.f;

    static inline auto BotFieldOfView = 110.f;

    static inline auto BotHearingRange = 800.f;

    static inline auto BotMemoryTicks = 60;

    static inline auto BotHuntPlayerRange = 24000.f;

    static inline auto BotPlayerPreference = 0.35f;

    static inline auto bBotsFightEachOther = true;

    static inline auto bBotsFightRealPlayers = true;

    static inline auto BotShootRealPlayerRange = 5000.f;

    static inline auto BotLongRangeAimErrorDegrees = 9.6f;

    static inline auto BotShotAggroTicks = 180;

    static inline auto BotAimErrorDegrees = 3.2f;

    static inline auto BotAimErrorPitchScale = 0.5f;

    static inline auto BotAimMissChance = 0.30f;

    static inline auto BotAimHitChance = 0.65f;        // 0.3 = sloppy, 0.65 = decent, 0.9 = sweaty

    static inline auto BotAimTurnSpeed = 6.0f;         // max degrees the aim turns per tick (30 ticks/s = 180 deg/s)
    static inline auto BotAimReactionTicks = 14;       // ticks after spotting someone before the aim even starts to move (~0.5 s)
    static inline auto BotAimTrackingLag = 0.30f;      // 1.0 = sees the target's exact position, lower = lags behind moving targets
    static inline auto BotAimOvershoot = 0.20f;        // how far past the target the first turn swings (fraction of the turn)
    static inline auto BotAimJitterDegrees = 0.35f;    // constant small hand shake
    static inline auto BotAimMovingPenalty = 1.2f;     // extra spread while the bot itself is running

    static inline auto BotWallDistance = 1024.f;

    static inline auto ElimCreditRange = 25000.f;

    static inline auto BotShieldMinimum = 25.f;

    static inline auto BotShieldMaximum = 100.f;

    static inline auto GrapplerClipSize = 999;

    static inline auto bGodModeForRealPlayer = false;
    static inline auto bFallDamage = true;

    static inline auto bLateGame = false;
    static inline auto LateGameZone = 3;
    static inline auto bLateGameLongZone = false;
    static inline auto bEnableCheats = true;
    static inline auto SiphonAmount = 0;
    static inline auto bInfiniteMats = false;
    static inline auto bInfiniteAmmo = false;
    static inline auto bForceRespawns = false;
    static inline auto bJoinInProgress = false;
    static inline auto bAutoRestart = false;

    static inline auto bKeepInventory = false;
    static inline auto AutoFillTargetPlayerCount = 100;

    static inline auto HotDropPoi = "";

    static inline auto HotDropBots = 0;

    static inline auto HotDropRadius = 9000.f;
    static inline auto AutoFillBatchSize = 5;
    static inline auto AutoFillIntervalSeconds = 2;
    static inline auto Port = 7777;
    static inline auto bEnableIris = true;

    static inline auto bSpawnBossesAndHenchmen = false;

    static inline auto HenchmenPerPOI = 6;

    static inline auto bForceBossAtEveryPOI = false;

    static inline auto MaxNPCs = 40;

    static inline auto bNPCLog = true;
    static inline constexpr auto bGUI = false;
    static inline constexpr auto bCustomCrashReporter = true;
    static inline constexpr auto bUseStdoutLog = false;
    static inline constexpr auto WebhookURL = "";
};
