#pragma once
#include "../../pch.h"

namespace ServerConsole
{
    void Start();

    void Say(const char* Format, ...);

    void SayOnce(const char* Key, const char* Format, ...);

    const char* LogFolder();
}
