#include "pch.h"
#include "../Public/PlaceDump.h"
#include "BakedPlaces.h"
#include "../Public/Configuration.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortPlayerPawnAthena.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <direct.h>

namespace PlaceDump
{
    struct FPlace
    {
        float X = 0.f;
        float Y = 0.f;
        float Z = 0.f;
        std::vector<int> Neighbours;
    };

    static std::vector<FPlace> GPlaces;

    struct FLandmark
    {
        std::string Name;
        float X = 0.f;
        float Y = 0.f;
        float Z = 0.f;
        int Places = 0;
    };

    static std::vector<FLandmark> GLandmarks;

    static std::string Simplify(const std::string& In)
    {
        std::string Out;

        for (size_t i = 0; i < In.size(); i++)
        {
            const unsigned char Ch = (unsigned char)In[i];

            if (Ch == ' ' || Ch == '_' || Ch == '-' || Ch == '\'')
                continue;

            if (Ch >= 'A' && Ch <= 'Z')
                Out += (char)(Ch - 'A' + 'a');
            else
                Out += (char)Ch;
        }

        return Out;
    }

    static std::unordered_map<long long, std::vector<int>> GGrid;

    static bool GLoaded = false;
    static bool GReported = false;

    static int GConnectedCount = 0;
    static int GIslandCount = 0;

    static const float CellSize = 250.f;

    static const float LinkRadius = 1100.f;

    static const float LinkHeight = 220.f;

    static long long CellKey(float X, float Y)
    {
        const long long CX = (long long)floorf(X / CellSize);
        const long long CY = (long long)floorf(Y / CellSize);

        return (CX << 32) ^ (CY & 0xFFFFFFFFLL);
    }

    static float DistanceSquared2D(const FPlace& A, float X, float Y)
    {
        const float DX = A.X - X;
        const float DY = A.Y - Y;
        return DX * DX + DY * DY;
    }

    static FILE* GFile = nullptr;
    static int GRunNumber = 0;
    static float GLastProgressTime = -1000.f;
    static float GLastRecordTime = -1000.f;
    static float GLastBotRecordTime = -1000.f;

    struct FLastPosition
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        bool bHave = false;
    };

    static std::unordered_map<void*, FLastPosition> GLastByPawn;
    static int GRecordedThisSession = 0;

    static void EnsureFolder()
    {
        std::string Folder = FConfiguration::PlaceDumpFolder;

        std::string Partial;
        for (size_t i = 0; i < Folder.size(); i++)
        {
            Partial += Folder[i];

            if ((Folder[i] == '\\' || Folder[i] == '/') && Partial.size() > 3)
                _mkdir(Partial.c_str());
        }

        _mkdir(Folder.c_str());
    }

    static int GNextRunNumber = 0;

    static std::string DumpFolderWithSlash()
    {
        std::string Folder = FConfiguration::PlaceDumpFolder;

        if (!Folder.empty() && Folder.back() != '\\' && Folder.back() != '/')
            Folder += "\\";

        return Folder;
    }

    static void FindNextRunNumber()
    {
        if (GNextRunNumber != 0)
            return;

        GNextRunNumber = 1;

        const std::string Pattern = DumpFolderWithSlash() + "Play*.txt";

        WIN32_FIND_DATAA Find{};
        HANDLE Handle = FindFirstFileA(Pattern.c_str(), &Find);

        if (Handle == INVALID_HANDLE_VALUE)
            return;

        do
        {
            int Number = 0;
            if (sscanf_s(Find.cFileName, "Play%d.txt", &Number) == 1 && Number >= GNextRunNumber)
                GNextRunNumber = Number + 1;
        }
        while (FindNextFileA(Handle, &Find));

        FindClose(Handle);
    }

    static void StartRun()
    {
        if (GFile)
            return;

        EnsureFolder();
        FindNextRunNumber();

        char Name[64];
        sprintf_s(Name, sizeof(Name), "Play%03d.txt", GNextRunNumber);

        const std::string Path = DumpFolderWithSlash() + Name;

        fopen_s(&GFile, Path.c_str(), "w");

        if (!GFile)
        {
            printf("[PlaceDump] Could not open %s for writing. Recording did not start.\n", Path.c_str());
            fflush(stdout);
            return;
        }

        GRunNumber = GNextRunNumber;
        GNextRunNumber++;
        GRecordedThisSession = 0;
        GLastByPawn.clear();

        SYSTEMTIME Time{};
        GetLocalTime(&Time);

        fprintf(GFile,
                "# Play %03d -- started %04d-%02d-%02d %02d:%02d:%02d\n",
                GRunNumber, Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
        fflush(GFile);

        printf("\n[PlaceDump] ======== RECORDING STARTED -- Play%03d ========\n", GRunNumber);
        printf("[PlaceDump] Writing to %s\n", Path.c_str());
        printf("[PlaceDump] Press F4 again to stop. Walk it the way you would actually play it.\n\n");
        fflush(stdout);
    }

    static void StopRun(const char* Why)
    {
        if (!GFile)
            return;

        SYSTEMTIME Time{};
        GetLocalTime(&Time);

        fprintf(GFile, "# Play %03d -- ended %02d:%02d:%02d, %d position(s)\n",
                GRunNumber, Time.wHour, Time.wMinute, Time.wSecond, GRecordedThisSession);

        fflush(GFile);
        fclose(GFile);
        GFile = nullptr;

        printf("\n[PlaceDump] ======== RECORDING STOPPED -- Play%03d ========\n", GRunNumber);
        printf("[PlaceDump] %d position(s) written%s%s.\n",
               GRecordedThisSession, Why ? " -- " : "", Why ? Why : "");
        printf("[PlaceDump] Press F4 to start Play%03d.\n\n", GNextRunNumber);
        fflush(stdout);

        GRecordedThisSession = 0;
    }

    static void PollHotkey()
    {
        if (!FConfiguration::bRecordPlaceDump)
            return;

        static bool bWasDown = false;

        const bool bDown = (GetAsyncKeyState(FConfiguration::PlaceDumpHotkey) & 0x8000) != 0;

        if (bDown && !bWasDown)
        {
            if (GFile)
                StopRun("stopped with F4");
            else
                StartRun();
        }

        bWasDown = bDown;
    }

    static bool IsOnGround(AFortPlayerPawnAthena* Pawn, float& OutSpeed)
    {
        OutSpeed = 0.f;

        if (!Pawn || !Pawn->HasCharacterMovement() || !Pawn->CharacterMovement)
            return false;

        auto Move = Pawn->CharacterMovement;

        FVector Velocity{};
        if (Move->HasVelocity())
            Velocity = Move->Velocity;

        OutSpeed = (float)sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y);

        if (Move->HasMovementMode())
        {
            const uint8 Mode = (uint8)Move->MovementMode;
            return Mode == 1 || Mode == 2;
        }

        return fabs(Velocity.Z) < 60.0;
    }

    void RecordTick()
    {
        if (!FConfiguration::bRecordPlaceDump)
            return;

        PollHotkey();

        if (!GFile)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        float Interval = FConfiguration::PlaceDumpIntervalSeconds;
        if (!(Interval > 0.f))
            Interval = 0.1f;

        if (Now - GLastRecordTime < Interval)
            return;
        GLastRecordTime = Now;

        if (Now - GLastProgressTime >= 10.f)
        {
            GLastProgressTime = Now;

            if (GRecordedThisSession > 0)
            {
                printf("[PlaceDump] Play%03d recording -- %d position(s) so far.\n", GRunNumber, GRecordedThisSession);
                fflush(stdout);
            }
        }

        bool bBotsDueNow = false;
        if (FConfiguration::bRecordBots)
        {
            float BotInterval = FConfiguration::PlaceDumpBotIntervalSeconds;
            if (!(BotInterval > 0.f))
                BotInterval = 1.f;

            if (Now - GLastBotRecordTime >= BotInterval)
            {
                GLastBotRecordTime = Now;
                bBotsDueNow = true;
            }
        }

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            const bool bIsBot = PS->HasbIsABot() && PS->bIsABot;

            if (bIsBot && !bBotsDueNow)
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            FVector Location = Pawn->K2_GetActorLocation();

            float Speed = 0.f;
            const bool bGround = IsOnGround(Pawn, Speed);

            FLastPosition& Last = GLastByPawn[(void*)Pawn];

            const float MinSpacing = bIsBot ? FConfiguration::PlaceDumpBotMinSpacing
                                            : FConfiguration::PlaceDumpMinSpacing;

            if (Last.bHave)
            {
                const double DX = Location.X - Last.X;
                const double DY = Location.Y - Last.Y;
                const double DZ = Location.Z - Last.Z;

                if ((DX * DX + DY * DY + DZ * DZ) < (double)(MinSpacing * MinSpacing))
                    continue;
            }

            Last.X = Location.X;
            Last.Y = Location.Y;
            Last.Z = Location.Z;
            Last.bHave = true;

            const double FromMiddle = sqrt(Location.X * Location.X + Location.Y * Location.Y);

            fprintf(GFile,
                    "location: X=%.2f Y=%.2f Z=%.2f | ground=%d speed=%.1f | dist=%.1f t=%.2f\n",
                    Location.X, Location.Y, Location.Z, bGround ? 1 : 0, Speed, FromMiddle, Now);

            GRecordedThisSession++;

            if ((GRecordedThisSession % 25) == 0)
            {
                fflush(GFile);

                static int Announced = 0;
                if (GRecordedThisSession >= 250 && (GRecordedThisSession % 250) == 0 && ++Announced <= 40)
                    printf("[PlaceDump] %d places recorded this session.\n", GRecordedThisSession);
            }

            return;
        }
    }

    void Flush()
    {
        StopRun("the match ended");
    }

    static int AddPlace(float X, float Y, float Z)
    {
        const long long Key = CellKey(X, Y);

        auto Cell = GGrid.find(Key);
        if (Cell != GGrid.end())
        {
            for (int Index : Cell->second)
            {
                const FPlace& Existing = GPlaces[Index];

                if (fabsf(Existing.Z - Z) < LinkHeight)
                    return Index;
            }
        }

        FPlace Place;
        Place.X = X;
        Place.Y = Y;
        Place.Z = Z;

        GPlaces.push_back(Place);
        GGrid[Key].push_back((int)GPlaces.size() - 1);

        return (int)GPlaces.size() - 1;
    }

    static void JoinPlaces(int A, int B)
    {
        if (A < 0 || B < 0 || A == B || A >= (int)GPlaces.size() || B >= (int)GPlaces.size())
            return;

        auto& From = GPlaces[A].Neighbours;
        for (int Existing : From)
            if (Existing == B)
                return;

        GPlaces[A].Neighbours.push_back(B);
        GPlaces[B].Neighbours.push_back(A);
    }

    static bool ParseLine(const char* Line, float& X, float& Y, float& Z, bool& bGround)
    {
        const char* PX = strstr(Line, "X=");
        const char* PY = strstr(Line, "Y=");
        const char* PZ = strstr(Line, "Z=");

        if (!PX || !PY || !PZ)
            return false;

        X = (float)atof(PX + 2);
        Y = (float)atof(PY + 2);
        Z = (float)atof(PZ + 2);

        const char* PG = strstr(Line, "ground=");
        bGround = PG ? (atoi(PG + 7) != 0) : true;

        if (X == 0.f && Y == 0.f && Z == 0.f)
            return false;

        return true;
    }

    static void LinkPlaces()
    {
        const float LinkRadiusSq = LinkRadius * LinkRadius;

        std::vector<std::pair<int, int>> Pairs;

        for (int i = 0; i < (int)GPlaces.size(); i++)
        {
            FPlace& Place = GPlaces[i];

            const long long CX = (long long)floorf(Place.X / CellSize);
            const long long CY = (long long)floorf(Place.Y / CellSize);

            const int Reach = (int)ceilf(LinkRadius / CellSize);

            for (int OX = -Reach; OX <= Reach; OX++)
            {
                for (int OY = -Reach; OY <= Reach; OY++)
                {
                    const long long Key = ((CX + OX) << 32) ^ ((CY + OY) & 0xFFFFFFFFLL);

                    auto Cell = GGrid.find(Key);
                    if (Cell == GGrid.end())
                        continue;

                    for (int Other : Cell->second)
                    {
                        if (Other == i)
                            continue;

                        const FPlace& That = GPlaces[Other];

                        if (fabsf(That.Z - Place.Z) > LinkHeight)
                            continue;

                        if (DistanceSquared2D(That, Place.X, Place.Y) > LinkRadiusSq)
                            continue;

                        Pairs.push_back({ i, Other });
                    }
                }
            }
        }

        for (const auto& Pair : Pairs)
            JoinPlaces(Pair.first, Pair.second);

        GConnectedCount = 0;

        {
            std::vector<int> Island((int)GPlaces.size(), -1);
            int NextIsland = 0;
            std::vector<int> Stack;

            for (int Start = 0; Start < (int)GPlaces.size(); Start++)
            {
                if (Island[Start] != -1)
                    continue;

                const int Id = NextIsland++;
                int Size = 0;

                Stack.clear();
                Stack.push_back(Start);
                Island[Start] = Id;

                while (!Stack.empty())
                {
                    const int Here = Stack.back();
                    Stack.pop_back();
                    Size++;

                    for (int Neighbour : GPlaces[Here].Neighbours)
                    {
                        if (Island[Neighbour] == -1)
                        {
                            Island[Neighbour] = Id;
                            Stack.push_back(Neighbour);
                        }
                    }
                }

                if (Size > GConnectedCount)
                    GConnectedCount = Size;
            }

            GIslandCount = NextIsland;
        }
    }

    void EnsureLoaded()
    {
        if (GLoaded)
            return;
        GLoaded = true;

        if (!FConfiguration::bUsePlaceDump)
            return;

        int BakedPlaced = 0;

        for (int a = 0; a < BakedPlaces::AreaCount; a++)
        {
            const BakedPlaces::FArea& Area = BakedPlaces::Areas[a];

            if (!Area.Points || Area.Count <= 0)
                continue;

            int PreviousPlace = -1;
            float PreviousX = 0.f, PreviousY = 0.f, PreviousZ = 0.f;

            double SumX = 0.0, SumY = 0.0, SumZ = 0.0;

            for (int i = 0; i < Area.Count; i++)
            {
                const float X = Area.Points[i].X;
                const float Y = Area.Points[i].Y;
                const float Z = Area.Points[i].Z;

                const int ThisPlace = AddPlace(X, Y, Z);

                if (PreviousPlace >= 0)
                {
                    const float DX = X - PreviousX;
                    const float DY = Y - PreviousY;
                    const float DZ = Z - PreviousZ;

                    const float MaxStep = FConfiguration::PlaceDumpMaxStep;

                    if ((DX * DX + DY * DY + DZ * DZ) <= MaxStep * MaxStep)
                        JoinPlaces(PreviousPlace, ThisPlace);
                }

                PreviousPlace = ThisPlace;
                PreviousX = X;
                PreviousY = Y;
                PreviousZ = Z;

                SumX += X;
                SumY += Y;
                SumZ += Z;

                BakedPlaced++;
            }

            FLandmark Mark;
            Mark.Name = Area.Name;
            Mark.X = (float)(SumX / (double)Area.Count);
            Mark.Y = (float)(SumY / (double)Area.Count);
            Mark.Z = (float)(SumZ / (double)Area.Count);
            Mark.Places = Area.Count;

            GLandmarks.push_back(Mark);
        }

        int BakedRoutes = 0;

        for (int r = 0; r < BakedPlaces::RouteCount; r++)
        {
            const BakedPlaces::FArea& Route = BakedPlaces::Routes[r];

            if (!Route.Points || Route.Count <= 0)
                continue;

            int PreviousPlace = -1;
            float PreviousX = 0.f, PreviousY = 0.f, PreviousZ = 0.f;

            for (int i = 0; i < Route.Count; i++)
            {
                const float X = Route.Points[i].X;
                const float Y = Route.Points[i].Y;
                const float Z = Route.Points[i].Z;

                const int ThisPlace = AddPlace(X, Y, Z);

                if (PreviousPlace >= 0)
                {
                    const float DX = X - PreviousX;
                    const float DY = Y - PreviousY;
                    const float DZ = Z - PreviousZ;

                    const float MaxStep = FConfiguration::PlaceDumpMaxStep;

                    if ((DX * DX + DY * DY + DZ * DZ) <= MaxStep * MaxStep)
                        JoinPlaces(PreviousPlace, ThisPlace);
                }

                PreviousPlace = ThisPlace;
                PreviousX = X;
                PreviousY = Y;
                PreviousZ = Z;

                BakedRoutes++;
            }
        }

        if (BakedPlaced > 0)
        {
            printf("[PlaceDump] %d place(s) across %d known area(s) are built into the server, "
                   "joined by %d place(s) of walked route.\n",
                   BakedPlaced, BakedPlaces::AreaCount, BakedRoutes);
            fflush(stdout);
        }

        std::string Folder = FConfiguration::PlaceDumpFolder;
        if (!Folder.empty() && Folder.back() != '\\' && Folder.back() != '/')
            Folder += "\\";

        std::string Pattern = Folder + "*.txt";

        WIN32_FIND_DATAA Find{};
        HANDLE Handle = FindFirstFileA(Pattern.c_str(), &Find);

        int FilesRead = 0;
        int LinesRead = 0;
        int Skipped = 0;

        if (Handle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (Find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;

                std::string Path = Folder + Find.cFileName;

                FILE* In = nullptr;
                fopen_s(&In, Path.c_str(), "r");
                if (!In)
                    continue;

                FilesRead++;

                bool bPoiFile = false;

                std::string FileName = Find.cFileName;
                const size_t Dot = FileName.find_last_of('.');

                if (Dot != std::string::npos)
                    FileName = FileName.substr(0, Dot);

                double SumX = 0.0, SumY = 0.0, SumZ = 0.0;
                int Kept = 0;

                int PreviousPlace = -1;
                float PreviousX = 0.f, PreviousY = 0.f, PreviousZ = 0.f;

                char Line[512];
                while (fgets(Line, sizeof(Line), In))
                {
                    if (Line[0] == '#' || Line[0] == '\n' || Line[0] == '\r')
                    {
                        if (Line[0] == '#' && strstr(Line, "POI"))
                            bPoiFile = true;

                        PreviousPlace = -1;
                        continue;
                    }

                    if (strncmp(Line, "location:", 9) != 0)
                        continue;

                    float X = 0.f, Y = 0.f, Z = 0.f;
                    bool bGround = true;

                    if (!ParseLine(Line, X, Y, Z, bGround))
                        continue;

                    LinesRead++;

                    const bool bTooHigh = Z > FConfiguration::PlaceDumpHeightCeiling;

                    if (bTooHigh || (!bGround && !bPoiFile))
                    {
                        Skipped++;
                        PreviousPlace = -1;
                        continue;
                    }

                    const int ThisPlace = AddPlace(X, Y, Z);

                    SumX += X;
                    SumY += Y;
                    SumZ += Z;
                    Kept++;

                    if (PreviousPlace >= 0)
                    {
                        const float DX = X - PreviousX;
                        const float DY = Y - PreviousY;
                        const float DZ = Z - PreviousZ;
                        const float StepSq = DX * DX + DY * DY + DZ * DZ;

                        const float MaxStep = FConfiguration::PlaceDumpMaxStep;

                        if (StepSq <= MaxStep * MaxStep)
                            JoinPlaces(PreviousPlace, ThisPlace);
                    }

                    PreviousPlace = ThisPlace;
                    PreviousX = X;
                    PreviousY = Y;
                    PreviousZ = Z;
                }

                fclose(In);

                if (bPoiFile && Kept > 0)
                {
                    FLandmark Mark;
                    Mark.Name = FileName;
                    Mark.X = (float)(SumX / (double)Kept);
                    Mark.Y = (float)(SumY / (double)Kept);
                    Mark.Z = (float)(SumZ / (double)Kept);
                    Mark.Places = Kept;

                    GLandmarks.push_back(Mark);
                }
            }
            while (FindNextFileA(Handle, &Find));

            FindClose(Handle);
        }

        if (GPlaces.empty())
        {
            printf("[PlaceDump] No places at all -- neither the ones built in nor anything in %s. "
                   "Only the hot drop uses them; without them it can only use a POI volume, if the map has a usable one.\n", Folder.c_str());
            fflush(stdout);
            return;
        }

        LinkPlaces();

        printf("[PlaceDump] Read %d file(s), %d recorded position(s) (%d skipped as mid-air).\n",
               FilesRead, LinesRead, Skipped);
        fflush(stdout);
    }

    int GetNodeCount()
    {
        return (int)GPlaces.size();
    }

    int GetConnectedNodeCount()
    {
        return GConnectedCount;
    }

    bool IsUsable()
    {
        return FConfiguration::bUsePlaceDump && GConnectedCount >= 16;
    }

    int GetSpreadLandingSpots(int Count, FVector* OutSpots)
    {
        EnsureLoaded();

        if (Count <= 0 || !OutSpots || GPlaces.empty())
            return 0;

        const int PlaceCount = (int)GPlaces.size();

        std::vector<int> Candidates;
        Candidates.reserve(PlaceCount);

        for (int i = 0; i < PlaceCount; i++)
        {
            if (!GPlaces[i].Neighbours.empty())
                Candidates.push_back(i);
        }

        if (Candidates.empty())
        {
            for (int i = 0; i < PlaceCount; i++)
                Candidates.push_back(i);
        }

        std::vector<int> Picked;
        Picked.reserve(Count);

        Picked.push_back(Candidates[(size_t)(rand() % (int)Candidates.size())]);

        const int SampleSize = 512;

        while ((int)Picked.size() < Count)
        {
            int Best = -1;
            float BestDistance = -1.f;

            for (int s = 0; s < SampleSize; s++)
            {
                const int Index = Candidates[(size_t)(rand() % (int)Candidates.size())];
                const FPlace& Here = GPlaces[Index];

                float Nearest = 1e30f;

                for (int P : Picked)
                {
                    const float D = DistanceSquared2D(GPlaces[P], Here.X, Here.Y);
                    if (D < Nearest)
                        Nearest = D;
                }

                if (Nearest > BestDistance)
                {
                    BestDistance = Nearest;
                    Best = Index;
                }
            }

            if (Best < 0)
                break;

            Picked.push_back(Best);
        }

        int Written = 0;

        for (int Index : Picked)
        {
            if (Written >= Count)
                break;

            const FPlace& P = GPlaces[Index];

            OutSpots[Written] = FVector{};
            OutSpots[Written].X = P.X;
            OutSpots[Written].Y = P.Y;
            OutSpots[Written].Z = P.Z;
            Written++;
        }

        return Written;
    }

    bool GetLandmark(const char* Name, FVector& Out)
    {
        EnsureLoaded();

        if (!Name || !*Name || GLandmarks.empty())
            return false;

        const std::string Wanted = Simplify(Name);

        if (Wanted.empty())
            return false;

        for (size_t i = 0; i < GLandmarks.size(); i++)
        {
            const std::string Have = Simplify(GLandmarks[i].Name);

            if (Have == Wanted)
            {
                Out = FVector{};
                Out.X = GLandmarks[i].X;
                Out.Y = GLandmarks[i].Y;
                Out.Z = GLandmarks[i].Z;
                return true;
            }
        }

        for (size_t i = 0; i < GLandmarks.size(); i++)
        {
            const std::string Have = Simplify(GLandmarks[i].Name);

            if (Have.find(Wanted) != std::string::npos)
            {
                Out = FVector{};
                Out.X = GLandmarks[i].X;
                Out.Y = GLandmarks[i].Y;
                Out.Z = GLandmarks[i].Z;
                return true;
            }
        }

        return false;
    }

    int GetPlacesNear(const FVector& Near, float WithinDistance, int Count, FVector* OutSpots)
    {
        EnsureLoaded();

        if (Count <= 0 || !OutSpots || GPlaces.empty())
            return 0;

        const float RadiusSq = WithinDistance * WithinDistance;

        std::vector<int> Candidates;

        for (size_t i = 0; i < GPlaces.size(); i++)
        {
            const float DX = GPlaces[i].X - (float)Near.X;
            const float DY = GPlaces[i].Y - (float)Near.Y;

            if ((DX * DX + DY * DY) <= RadiusSq)
                Candidates.push_back((int)i);
        }

        if (Candidates.empty())
            return 0;

        int Written = 0;

        const int Stride = (int)Candidates.size() / (Count > 0 ? Count : 1);
        const int Step = Stride > 0 ? Stride : 1;

        int Index = rand() % (int)Candidates.size();

        while (Written < Count)
        {
            const FPlace& P = GPlaces[Candidates[(size_t)(Index % (int)Candidates.size())]];

            OutSpots[Written] = FVector{};
            OutSpots[Written].X = P.X;
            OutSpots[Written].Y = P.Y;
            OutSpots[Written].Z = P.Z;
            Written++;

            Index += Step;
        }

        return Written;
    }

    void ReportOnce()
    {
        if (GReported)
            return;
        GReported = true;

        for (size_t i = 0; i < GLandmarks.size(); i++)
            printf("[PlaceDump] Landmark \"%s\": %d place(s), middle X=%.0f Y=%.0f Z=%.0f\n",
                   GLandmarks[i].Name.c_str(), GLandmarks[i].Places,
                   GLandmarks[i].X, GLandmarks[i].Y, GLandmarks[i].Z);

        if (GPlaces.empty())
            return;

        const int Total = (int)GPlaces.size();
        const float Reachable = Total > 0 ? (100.f * (float)GConnectedCount / (float)Total) : 0.f;

        printf("[PlaceDump] %d places recorded, in %d separate area(s). The biggest holds %d of them (%.0f%%).\n",
               Total, GIslandCount, GConnectedCount, Reachable);

        printf("[PlaceDump] Bots do NOT follow these routes any more -- movement is straight-line; "
               "the recorded places are only used to find the hot-drop spot.\n");

        fflush(stdout);
    }

    void AnnounceHotkey()
    {
        static bool bSaid = false;
        if (bSaid || !FConfiguration::bRecordPlaceDump)
            return;
        bSaid = true;

        FindNextRunNumber();

        printf("[PlaceDump] Press F4 when you are on the ground and playing properly -- that starts Play%03d.\n"
               "[PlaceDump] Press it again to stop. Nothing is recorded until you do.\n",
               GNextRunNumber);
        fflush(stdout);
    }

    static int FindNearestPlace(const FVector& To, float MaxDistance)
    {
        if (GPlaces.empty())
            return -1;

        const float X = (float)To.X;
        const float Y = (float)To.Y;
        const float Z = (float)To.Z;

        const long long CX = (long long)floorf(X / CellSize);
        const long long CY = (long long)floorf(Y / CellSize);

        int Best = -1;
        float BestScore = MaxDistance * MaxDistance;

        const int MaxRing = (int)ceilf(MaxDistance / CellSize);

        for (int Ring = 0; Ring <= MaxRing; Ring++)
        {
            for (int OX = -Ring; OX <= Ring; OX++)
            {
                for (int OY = -Ring; OY <= Ring; OY++)
                {
                    if (Ring > 0 && abs(OX) != Ring && abs(OY) != Ring)
                        continue;

                    const long long Key = ((CX + OX) << 32) ^ ((CY + OY) & 0xFFFFFFFFLL);

                    auto Cell = GGrid.find(Key);
                    if (Cell == GGrid.end())
                        continue;

                    for (int Index : Cell->second)
                    {
                        const FPlace& Place = GPlaces[Index];

                        const float DX = Place.X - X;
                        const float DY = Place.Y - Y;
                        const float DZ = (Place.Z - Z) * 2.f;

                        const float Score = DX * DX + DY * DY + DZ * DZ;

                        if (Score < BestScore)
                        {
                            BestScore = Score;
                            Best = Index;
                        }
                    }
                }
            }

            if (Best != -1 && Ring >= 1)
                break;
        }

        return Best;
    }

    struct FRoute
    {
        std::vector<int> Places;
        size_t Next = 0;
        float GoalX = 0.f;
        float GoalY = 0.f;
    };

    static std::map<void*, FRoute> GRoutes;

    static int GRouteBudgetThisTick = 0;
    static int GRouteBudgetTick = -1;

    static float Heuristic(const FPlace& A, const FPlace& B)
    {
        const float DX = A.X - B.X;
        const float DY = A.Y - B.Y;
        const float DZ = A.Z - B.Z;
        return sqrtf(DX * DX + DY * DY + DZ * DZ);
    }

    static float StepPreference(int Node, unsigned int RouteSeed)
    {
        if (RouteSeed == 0)
            return 1.f;

        unsigned int Hash = (unsigned int)Node * 2654435761u;
        Hash ^= RouteSeed + 0x9E3779B9u + (Hash << 6) + (Hash >> 2);
        Hash ^= Hash >> 15;
        Hash *= 2246822519u;
        Hash ^= Hash >> 13;

        return 0.85f + (float)(Hash % 301u) * 0.001f;
    }

    static bool BuildRoute(int StartIndex, int GoalIndex, std::vector<int>& OutPlaces, unsigned int RouteSeed = 0)
    {
        if (StartIndex < 0 || GoalIndex < 0 || StartIndex >= (int)GPlaces.size() || GoalIndex >= (int)GPlaces.size())
            return false;

        if (StartIndex == GoalIndex)
        {
            OutPlaces.clear();
            OutPlaces.push_back(GoalIndex);
            return true;
        }

        const int MaxExpansions = 40000;

        const int PlaceCount = (int)GPlaces.size();

        static std::vector<float> CostSoFar;
        static std::vector<int> CameFrom;
        static std::vector<unsigned int> Stamp;
        static unsigned int SearchStamp = 0;

        if ((int)Stamp.size() != PlaceCount)
        {
            CostSoFar.assign(PlaceCount, 0.f);
            CameFrom.assign(PlaceCount, -1);
            Stamp.assign(PlaceCount, 0);
            SearchStamp = 0;
        }

        SearchStamp++;

        if (SearchStamp == 0)
        {
            Stamp.assign(PlaceCount, 0);
            SearchStamp = 1;
        }

        struct FOpen
        {
            float Estimate;
            int Index;
            bool operator<(const FOpen& Other) const { return Estimate > Other.Estimate; }
        };

        std::priority_queue<FOpen> Open;

        CostSoFar[StartIndex] = 0.f;
        CameFrom[StartIndex] = -1;
        Stamp[StartIndex] = SearchStamp;

        Open.push({ Heuristic(GPlaces[StartIndex], GPlaces[GoalIndex]), StartIndex });

        int Expansions = 0;
        bool bFound = false;

        int BestEffort = StartIndex;
        float BestEffortDistance = Heuristic(GPlaces[StartIndex], GPlaces[GoalIndex]);

        while (!Open.empty())
        {
            const FOpen Current = Open.top();
            Open.pop();

            if (Current.Index == GoalIndex)
            {
                bFound = true;
                break;
            }

            {
                const float ToGoal = Heuristic(GPlaces[Current.Index], GPlaces[GoalIndex]);
                if (ToGoal < BestEffortDistance)
                {
                    BestEffortDistance = ToGoal;
                    BestEffort = Current.Index;
                }
            }

            if (++Expansions > MaxExpansions)
                break;

            const FPlace& Here = GPlaces[Current.Index];
            const float HereCost = CostSoFar[Current.Index];

            for (int Neighbour : Here.Neighbours)
            {
                const float Step = Heuristic(Here, GPlaces[Neighbour]) * StepPreference(Neighbour, RouteSeed);
                const float NewCost = HereCost + Step;

                if (Stamp[Neighbour] == SearchStamp && CostSoFar[Neighbour] <= NewCost)
                    continue;

                Stamp[Neighbour] = SearchStamp;
                CostSoFar[Neighbour] = NewCost;
                CameFrom[Neighbour] = Current.Index;

                Open.push({ NewCost + Heuristic(GPlaces[Neighbour], GPlaces[GoalIndex]), Neighbour });
            }
        }

        int Target = GoalIndex;

        if (!bFound)
        {
            if (BestEffort == StartIndex)
                return false;

            Target = BestEffort;
        }

        OutPlaces.clear();

        int Walk = Target;
        for (int Guard = 0; Guard < 4096; Guard++)
        {
            OutPlaces.push_back(Walk);

            if (Walk == StartIndex)
                break;

            if (Walk < 0 || Walk >= PlaceCount || Stamp[Walk] != SearchStamp)
                break;

            const int Previous = CameFrom[Walk];
            if (Previous < 0)
                break;

            Walk = Previous;
        }

        for (size_t i = 0, j = OutPlaces.size() ? OutPlaces.size() - 1 : 0; i < j; i++, j--)
        {
            const int Swap = OutPlaces[i];
            OutPlaces[i] = OutPlaces[j];
            OutPlaces[j] = Swap;
        }

        return OutPlaces.size() > 1;
    }

    bool GetNextWaypoint(void* Identity, const FVector& From, const FVector& To, FVector& OutWaypoint)
    {
        if (!IsUsable() || !Identity)
            return false;

        auto& Route = GRoutes[Identity];

        const bool bGoalMoved = (fabs((double)Route.GoalX - To.X) > 2000.0) || (fabs((double)Route.GoalY - To.Y) > 2000.0);
        const bool bNoRoute = Route.Places.empty() || Route.Next >= Route.Places.size();

        if (bGoalMoved || bNoRoute)
        {
            static int LastTick = -1;
            const int Tick = (int)(GetTickCount64() / 16);

            if (Tick != GRouteBudgetTick)
            {
                GRouteBudgetTick = Tick;
                GRouteBudgetThisTick = 0;
            }

            if (GRouteBudgetThisTick >= 4)
            {
                if (!Route.Places.empty() && Route.Next < Route.Places.size())
                {
                    const FPlace& Place = GPlaces[Route.Places[Route.Next]];
                    OutWaypoint = FVector{};
                    OutWaypoint.X = Place.X;
                    OutWaypoint.Y = Place.Y;
                    OutWaypoint.Z = Place.Z;
                    return true;
                }
                return false;
            }

            GRouteBudgetThisTick++;
            (void)LastTick;

            const int StartIndex = FindNearestPlace(From, 4000.f);
            const int GoalIndex = FindNearestPlace(To, 12000.f);

            Route.Places.clear();
            Route.Next = 0;
            Route.GoalX = (float)To.X;
            Route.GoalY = (float)To.Y;

            unsigned int RouteSeed = (unsigned int)(((uintptr_t)Identity >> 4) * 2654435761u);
            if (RouteSeed == 0)
                RouteSeed = 1;

            if (!BuildRoute(StartIndex, GoalIndex, Route.Places, RouteSeed))
                return false;

            Route.Next = 1;
        }

        for (int Guard = 0; Guard < 64 && Route.Next < Route.Places.size(); Guard++)
        {
            const FPlace& Place = GPlaces[Route.Places[Route.Next]];

            const double DX = Place.X - From.X;
            const double DY = Place.Y - From.Y;

            if ((DX * DX + DY * DY) > (400.0 * 400.0))
                break;

            Route.Next++;
        }

        if (Route.Next >= Route.Places.size())
            return false;

        const FPlace& Next = GPlaces[Route.Places[Route.Next]];

        OutWaypoint = FVector{};
        OutWaypoint.X = Next.X;
        OutWaypoint.Y = Next.Y;
        OutWaypoint.Z = Next.Z;

        return true;
    }

    void ForgetBot(void* Identity)
    {
        GRoutes.erase(Identity);
    }

    bool GetRandomNearbyPlace(const FVector& Near, float WithinDistance, FVector& OutPlace)
    {
        if (!IsUsable())
            return false;

        const long long CX = (long long)floorf((float)Near.X / CellSize);
        const long long CY = (long long)floorf((float)Near.Y / CellSize);

        int Reach = (int)ceilf(WithinDistance / CellSize);
        if (Reach <= 0)
            return false;
        if (Reach > 40)
            Reach = 40;

        std::vector<int> Candidates;
        Candidates.reserve(64);

        for (int Ring = 0; Ring <= Reach; Ring++)
        {
            for (int OX = -Ring; OX <= Ring; OX++)
            {
                for (int OY = -Ring; OY <= Ring; OY++)
                {
                    if (Ring > 0 && abs(OX) != Ring && abs(OY) != Ring)
                        continue;

                    const long long Key = ((CX + OX) << 32) ^ ((CY + OY) & 0xFFFFFFFFLL);

                    auto Cell = GGrid.find(Key);
                    if (Cell == GGrid.end())
                        continue;

                    for (int Index : Cell->second)
                    {
                        if (GPlaces[Index].Neighbours.empty())
                            continue;

                        Candidates.push_back(Index);

                        if (Candidates.size() >= 256)
                            break;
                    }

                    if (Candidates.size() >= 256)
                        break;
                }

                if (Candidates.size() >= 256)
                    break;
            }

            if (Candidates.size() >= 48 && Ring >= 6)
                break;

            if (Candidates.size() >= 256)
                break;
        }

        if (Candidates.empty())
            return false;

        const FPlace& Place = GPlaces[Candidates[rand() % Candidates.size()]];

        OutPlace = FVector{};
        OutPlace.X = Place.X;
        OutPlace.Y = Place.Y;
        OutPlace.Z = Place.Z;

        return true;
    }
}
