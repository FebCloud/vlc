/*****************************************************************************
 * StartupPolicy.hpp
 *****************************************************************************
 * Copyright (C) 2026 VideoLAN and VLC Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/
#ifndef STARTUPPOLICY_HPP
#define STARTUPPOLICY_HPP

#include <vlc_common.h>
#include <vlc_tick.h>

#include <atomic>

namespace adaptive
{
    namespace logic
    {
        class StartupPolicy
        {
            public:
                StartupPolicy(bool enabled, vlc_tick_t requestedMinimum,
                              bool parallelPreparation = false,
                              bool mediaPrefetch = false);

                vlc_tick_t getMinBuffering(vlc_tick_t upstreamMinimum) const;
                void onMediaDequeued();
                bool isStarting() const;
                bool shouldPrepareInParallel() const;
                bool shouldPrefetchMediaChunk() const;

            private:
                const vlc_tick_t minimum;
                const bool parallel;
                const bool media;
                std::atomic<bool> starting;
        };
    }
}

#endif
