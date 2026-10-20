#include "pch.h"
#include "../Public/ServerConsole.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include <direct.h>
#include <io.h>

namespace ServerConsole
{
    static HANDLE GConsole = INVALID_HANDLE_VALUE;
    static std::string GLogFolder;
    static std::string GLogFile;
    static std::set<std::string> GAlreadySaid;
    static bool GStarted = false;

    static CRITICAL_SECTION GLock;
    static bool GLockReady = false;

    struct FLine
    {
        FLine()
        {
            if (GLockReady)
                EnterCriticalSection(&GLock);
        }

        ~FLine()
        {
            if (GLockReady)
                LeaveCriticalSection(&GLock);
        }
    };

    static std::string BuildRoot()
    {
        char ExePath[MAX_PATH] = {};

        if (!GetModuleFileNameA(nullptr, ExePath, MAX_PATH))
            return std::string();

        std::string Path = ExePath;

        for (auto& Ch : Path)
        {
            if (Ch == '/')
                Ch = '\\';
        }

        std::string Lowered = Path;
        for (auto& Ch : Lowered)
            Ch = (char)tolower((unsigned char)Ch);

        const size_t Marker = Lowered.find("\\fortnitegame\\binaries\\");

        if (Marker != std::string::npos)
            return Path.substr(0, Marker);

        const size_t LastSlash = Path.find_last_of('\\');

        if (LastSlash == std::string::npos)
            return std::string();

        return Path.substr(0, LastSlash);
    }

    static void MakeFolders(const std::string& Folder)
    {
        std::string Partial;

        for (size_t i = 0; i < Folder.size(); i++)
        {
            Partial += Folder[i];

            if ((Folder[i] == '\\' || Folder[i] == '/') && Partial.size() > 3)
                _mkdir(Partial.c_str());
        }

        _mkdir(Folder.c_str());
    }

    const char* LogFolder()
    {
        return GLogFolder.c_str();
    }

    void Start()
    {
        if (GStarted)
            return;

        GStarted = true;

        InitializeCriticalSection(&GLock);
        GLockReady = true;

        GConsole = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        const std::string Root = BuildRoot();

        if (Root.empty())
            return;

        GLogFolder = Root + "\\FortniteGame\\Content\\Gameserver Logs";
        MakeFolders(GLogFolder);

        SYSTEMTIME Time{};
        GetLocalTime(&Time);

        char Name[MAX_PATH] = {};
        sprintf_s(Name, sizeof(Name), "%s\\Gameserver %04d-%02d-%02d %02d-%02d-%02d.log",
                  GLogFolder.c_str(), Time.wYear, Time.wMonth, Time.wDay,
                  Time.wHour, Time.wMinute, Time.wSecond);

        GLogFile = Name;

        FILE* Redirected = nullptr;

        if (freopen_s(&Redirected, GLogFile.c_str(), "w", stdout) == 0 && Redirected)
        {
            setvbuf(stdout, nullptr, _IOLBF, 8192);

            const int OutFd = _fileno(stdout);
            const int ErrFd = _fileno(stderr);

            if (OutFd >= 0 && ErrFd >= 0)
                _dup2(OutFd, ErrFd);

            setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }

    static void Write(const char* Text)
    {
        if (GConsole == INVALID_HANDLE_VALUE || !Text)
            return;

        DWORD Written = 0;
        WriteConsoleA(GConsole, Text, (DWORD)strlen(Text), &Written, nullptr);
    }

    void Say(const char* Format, ...)
    {
        if (!Format)
            return;

        char Buffer[1024];

        va_list Args;
        va_start(Args, Format);
        vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        FLine Line;

        Write(Buffer);
        Write("\r\n");

        printf("[console] %s\n", Buffer);
    }

    void SayOnce(const char* Key, const char* Format, ...)
    {
        if (!Key || !Format)
            return;

        char Buffer[1024];

        va_list Args;
        va_start(Args, Format);
        vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        FLine Line;

        if (GAlreadySaid.find(Key) != GAlreadySaid.end())
            return;

        GAlreadySaid.insert(Key);

        Write(Buffer);
        Write("\r\n");

        printf("[console] %s\n", Buffer);
    }
}
