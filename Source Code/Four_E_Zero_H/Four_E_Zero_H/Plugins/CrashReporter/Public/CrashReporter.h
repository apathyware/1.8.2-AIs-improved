#pragma once
#include "../../../../pch.h"

class FCrashReporter
{
public:
    static void Register();

    static void EnterGuardedSection();
    static void LeaveGuardedSection();

    static int GetRecoveredNullCallCount();

    struct FScopedGuard
    {
        FScopedGuard()
        {
            FCrashReporter::EnterGuardedSection();
        }
        ~FScopedGuard()
        {
            FCrashReporter::LeaveGuardedSection();
        }
    };
};
