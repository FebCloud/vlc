/*****************************************************************************
 * StartupPolicy.cpp
 *****************************************************************************
 * Copyright (C) 2026 VideoLAN and VLC Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "StartupPolicy.hpp"

#include <algorithm>

using namespace adaptive::logic;

StartupPolicy::StartupPolicy(bool enabled_, vlc_tick_t requestedMinimum,
                             bool parallelPreparation, bool mediaPrefetch) :
    minimum(std::max(CLOCK_FREQ / 2,
                     std::min(6 * CLOCK_FREQ, requestedMinimum))),
    parallel(enabled_ && parallelPreparation),
    media(enabled_ && parallelPreparation && mediaPrefetch),
    starting(enabled_)
{
}

vlc_tick_t StartupPolicy::getMinBuffering(vlc_tick_t upstreamMinimum) const
{
    if(!starting.load(std::memory_order_relaxed))
        return upstreamMinimum;
    return std::min(minimum, upstreamMinimum);
}

void StartupPolicy::onMediaDequeued()
{
    starting.store(false, std::memory_order_relaxed);
}

bool StartupPolicy::isStarting() const
{
    return starting.load(std::memory_order_relaxed);
}

bool StartupPolicy::shouldPrepareInParallel() const
{
    return parallel && isStarting();
}

bool StartupPolicy::shouldPrefetchMediaChunk() const
{
    return media && isStarting();
}
