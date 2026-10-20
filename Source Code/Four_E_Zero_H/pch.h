#ifndef PCH_H
#define PCH_H

#include "framework.h"
#include "../SDK/Includes.h"
using namespace SDK;
#include "Four_E_Zero_H/Public/Utils.h"
#include <numeric>
#include <algorithm>
#include "libcurl/curl.h"

inline UEAllocatedString iso8601()
{
    time_t now;
    time(&now);
    char buf[sizeof "2011-10-08T07:07:09Z"];
    tm* t = new tm();
    gmtime_s(t, &now);
    strftime(buf, sizeof buf, "%FT%TZ", t);
    return UEAllocatedString(buf);
}

#endif
