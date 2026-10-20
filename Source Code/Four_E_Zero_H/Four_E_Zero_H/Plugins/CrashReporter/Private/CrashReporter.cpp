#include "pch.h"
#include "../Public/CrashReporter.h"
#include <TlHelp32.h>
#include <sstream>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

void FreezeOtherThreads()
{
    auto thrHandle = GetCurrentThread();
    auto currentThr = GetThreadId(thrHandle);
    auto currentPrc = GetProcessIdOfThread(thrHandle);
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(h, &te))
        {
            do
            {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
                {
                    if (te.th32ThreadID != currentThr && te.th32OwnerProcessID == currentPrc)
                    {
                        auto thr = OpenThread(THREAD_ALL_ACCESS, false, te.th32ThreadID);

                        if (thr != INVALID_HANDLE_VALUE)
                        {
                            SuspendThread(thr);
                            CloseHandle(thr);
                        }
                    }
                }
                te.dwSize = sizeof(te);
            }
            while (Thread32Next(h, &te));
        }
        CloseHandle(h);
    }
}

DWORD FormatNtStatus(NTSTATUS nsCode, TCHAR** ppszMessage)
{
    HMODULE ntdll = LoadLibraryA("ntdll.dll");

    if (ntdll == NULL)
        return 0;

    DWORD outLen = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE, ntdll, RtlNtStatusToDosError(nsCode), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  (LPSTR)ppszMessage, 0, NULL);

    FreeLibrary(ntdll);

    return outLen;
}

static thread_local int GGuardDepth = 0;

static volatile long GRecoveredNullCalls = 0;

void FCrashReporter::EnterGuardedSection()
{
    ++GGuardDepth;
}

void FCrashReporter::LeaveGuardedSection()
{
    if (GGuardDepth > 0)
        --GGuardDepth;
}

int FCrashReporter::GetRecoveredNullCallCount()
{
    return (int)GRecoveredNullCalls;
}

static bool ReadPointerSafely(DWORD64 Address, DWORD64& OutValue)
{
    if (Address < 0x10000 || (Address & 7) != 0)
        return false;

    MEMORY_BASIC_INFORMATION Info{};

    if (VirtualQuery((LPCVOID)Address, &Info, sizeof(Info)) != sizeof(Info))
        return false;

    if (Info.State != MEM_COMMIT)
        return false;

    if (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    OutValue = *(DWORD64*)Address;
    return true;
}

LONG WINAPI four_e_zero_h_UnhandledExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    if ((ExceptionInfo->ExceptionRecord->ExceptionCode & 0x80000000) == 0 || (ExceptionInfo->ExceptionRecord->ExceptionCode & 0x30000000) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ExceptionInfo->ContextRecord->Rip == 0)
    {
        DWORD64 ReturnAddress = 0;

        if (GRecoveredNullCalls < 65536 && ReadPointerSafely(ExceptionInfo->ContextRecord->Rsp, ReturnAddress) && ReturnAddress > 0x10000)
        {
            const long Count = InterlockedIncrement(&GRecoveredNullCalls);

            if (Count <= 20 || (Count % 1000) == 0)
            {
                const DWORD64 Base = (DWORD64)GetModuleHandleW(nullptr);

                printf("[CrashReporter] Stepped over a call to a function this build does not have (caller at +0x%llX). The match carries on. [%ld]\n",
                       (ReturnAddress > Base) ? (ReturnAddress - Base) : ReturnAddress, Count);

                if (Count == 20)
                    printf("[CrashReporter] ...only every thousandth one gets printed from here on.\n");

                fflush(stdout);
            }

            ExceptionInfo->ContextRecord->Rip = ReturnAddress;
            ExceptionInfo->ContextRecord->Rsp += 8;
            ExceptionInfo->ContextRecord->Rax = 0;

            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (GGuardDepth > 0)
    {
        printf("[CrashReporter] Fault inside a guarded section (Code: 0x%08X) -- handing it to the guard instead of ending the match.\n",
               (unsigned int)ExceptionInfo->ExceptionRecord->ExceptionCode);
        fflush(stdout);

        return EXCEPTION_CONTINUE_SEARCH;
    }

    FreezeOtherThreads();

    STACKFRAME64 stackFrame{};

    char symName[1024 * sizeof(TCHAR)];
    char symStorage[sizeof(IMAGEHLP_SYMBOL64) + sizeof(symName)];
    auto sym = (IMAGEHLP_SYMBOL64*)symStorage;

    auto currentPrc = GetCurrentProcess();
    auto currentThr = GetCurrentThread();
    DWORD64 displacement = 0;
    stackFrame.AddrPC.Offset = ExceptionInfo->ContextRecord->Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = ExceptionInfo->ContextRecord->Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = ExceptionInfo->ContextRecord->Rbp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;

    SymCleanup(currentPrc);

    auto InitResult = SymInitialize(currentPrc, nullptr, true);

    if (!InitResult)
        printf("[CrashReporter] Failed to initialize symbol finder!");

    std::stringstream reportStream;

    reportStream << "[CrashReporter] Caught unhandled exception (Code: ";
    char code[9];

    switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
    {
#define NAMED_EX(x)                                                                                                                                                                                                   \
    case x:                                                                                                                                                                                                           \
        reportStream << #x;                                                                                                                                                                                           \
        break;

        NAMED_EX(EXCEPTION_ACCESS_VIOLATION);
        NAMED_EX(EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
        NAMED_EX(EXCEPTION_BREAKPOINT);
        NAMED_EX(EXCEPTION_DATATYPE_MISALIGNMENT);
        NAMED_EX(EXCEPTION_FLT_DENORMAL_OPERAND);
        NAMED_EX(EXCEPTION_FLT_DIVIDE_BY_ZERO);
        NAMED_EX(EXCEPTION_FLT_INEXACT_RESULT);
        NAMED_EX(EXCEPTION_FLT_INVALID_OPERATION);
        NAMED_EX(EXCEPTION_FLT_OVERFLOW);
        NAMED_EX(EXCEPTION_FLT_STACK_CHECK);
        NAMED_EX(EXCEPTION_FLT_UNDERFLOW);
        NAMED_EX(EXCEPTION_ILLEGAL_INSTRUCTION);
        NAMED_EX(EXCEPTION_INT_DIVIDE_BY_ZERO);
        NAMED_EX(EXCEPTION_INT_OVERFLOW);
        NAMED_EX(EXCEPTION_INVALID_DISPOSITION);
        NAMED_EX(EXCEPTION_NONCONTINUABLE_EXCEPTION);
        NAMED_EX(EXCEPTION_PRIV_INSTRUCTION);
        NAMED_EX(EXCEPTION_SINGLE_STEP);
        NAMED_EX(EXCEPTION_STACK_OVERFLOW);

#undef NAMED_EX
    default:
        snprintf(code, 9, "%08x", ExceptionInfo->ExceptionRecord->ExceptionCode);
        reportStream << code;
    }
    reportStream << ")";

    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
    {
        switch (ExceptionInfo->ExceptionRecord->ExceptionInformation[0])
        {
        case 0:
            reportStream << "\n- Trying to read ";
            break;
        case 1:
            reportStream << "\n- Trying to write ";
            break;
        case 8:
            reportStream << "\n- Trying to execute ";
            break;
        }
        if (ExceptionInfo->ExceptionRecord->ExceptionInformation[1] == 0xFFFFF78000000900) 
            return EXCEPTION_CONTINUE_SEARCH;

        char addr[19];
        snprintf(addr, 19, "0x%016llx", ExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
        reportStream << addr;
    }
    reportStream << "\n\n";

    for (int frame = 0;; frame++)
    {
        auto contextCpy = *ExceptionInfo->ContextRecord;
        contextCpy.ContextFlags = CONTEXT_ALL;

        auto result = StackWalk(IMAGE_FILE_MACHINE_AMD64, currentPrc, currentThr, &stackFrame, ExceptionInfo->ContextRecord, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL);

        if (result == false)
            break;

        char addr[19];
        snprintf(addr, 19, "0x%016llx", stackFrame.AddrPC.Offset);
        reportStream << addr;

        auto imageBase = SymGetModuleBase64(currentPrc, stackFrame.AddrPC.Offset);
        if (imageBase)
        {
            char path[1024];
            GetModuleFileNameA(HMODULE(imageBase), path, 1024);

            auto filteredPath = strrchr(path, '\\');

            char offsetStr[19];
            snprintf(offsetStr, 19, "0x%llx", stackFrame.AddrPC.Offset - imageBase);
            reportStream << " (" << (filteredPath ? filteredPath + 1 : path) << "+" << offsetStr << ")";
        }
        reportStream << ": ";

        sym->SizeOfStruct = sizeof(symStorage);
        sym->MaxNameLength = sizeof(symName);

        BOOL SymResult = SymGetSymFromAddr64(currentPrc, (ULONG64)stackFrame.AddrPC.Offset, &displacement, sym);
        if (SymResult == false || imageBase == ImageBase)
            reportStream << "[unknown]\n";
        else
        {
            UnDecorateSymbolName(sym->Name, (PSTR)symName, sizeof(symName), UNDNAME_COMPLETE);

            reportStream << sym->Name << "()";
            IMAGEHLP_LINE64 ImageHelpLine = { 0 };
            ImageHelpLine.SizeOfStruct = sizeof(ImageHelpLine);
            if (SymGetLineFromAddr64(currentPrc, (ULONG64)stackFrame.AddrPC.Offset, (::DWORD*)&displacement, &ImageHelpLine))
            {
                auto filteredName = strrchr(ImageHelpLine.FileName, '\\');
                reportStream << " [" << (filteredName ? filteredName + 1 : ImageHelpLine.FileName) << ":" << ImageHelpLine.LineNumber << "]";
            }
            reportStream << "\n";
        }
    }
    auto reportStr = reportStream.str();
    printf("%s", reportStr.c_str());

    Memcury::Util::CopyToClipboard(reportStr);
    SymCleanup(currentPrc);
    Sleep(3000);
    
    TerminateProcess(GetCurrentProcess(), ExceptionInfo->ExceptionRecord->ExceptionCode);
    
    return EXCEPTION_CONTINUE_EXECUTION;
}

void FCrashReporter::Register()
{
    AddVectoredExceptionHandler(0, four_e_zero_h_UnhandledExceptionFilter);
}
