#pragma once
#include <string>

namespace NoKillerMessages
{
    static const int Count = 5;

    inline int PickFor(const wchar_t* Name, int MaxLength = 512)
    {
        unsigned int Hash = 2166136261u;

        if (Name)
        {
            for (int i = 0; i < MaxLength && Name[i] != L'\0'; i++)
            {
                Hash ^= (unsigned int)(unsigned short)Name[i];
                Hash *= 16777619u;
            }
        }

        return (int)(Hash % (unsigned int)Count);
    }

    inline std::wstring Line(int Which, const std::wstring& Name)
    {
        switch (((Which % Count) + Count) % Count)
        {
        case 0:
            return Name + L" had an oopsie";
        case 1:
            return Name + L" took the L";
        case 2:
            return Name + L" took the easy way out";
        case 3:
            return L"looks like we lost " + Name;
        default:
            return Name + L" paid the 500 dollars";
        }
    }

    inline std::wstring LineForKillFeed(int Which, const std::wstring& Name)
    {
        return Line(Which, L"<Victim>" + Name + L"</>");
    }

    inline std::string LineUtf8(int Which, const std::string& Name)
    {
        switch (((Which % Count) + Count) % Count)
        {
        case 0:
            return Name + " had an oopsie";
        case 1:
            return Name + " took the L";
        case 2:
            return Name + " took the easy way out";
        case 3:
            return "looks like we lost " + Name;
        default:
            return Name + " paid the 500 dollars";
        }
    }
}
