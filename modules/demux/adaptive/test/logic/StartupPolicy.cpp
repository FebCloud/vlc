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

#include "../../logic/StartupPolicy.hpp"
#include "../test.hpp"

#include <vlc_common.h>

using namespace adaptive::logic;

int StartupPolicy_test()
{
    const vlc_tick_t upstream = 10 * CLOCK_FREQ;

    StartupPolicy disabled(false, 2 * CLOCK_FREQ);
    Expect(!disabled.isStarting());
    Expect(disabled.getMinBuffering(upstream) == upstream);

    StartupPolicy enabled(true, 2 * CLOCK_FREQ);
    Expect(enabled.isStarting());
    Expect(enabled.getMinBuffering(upstream) == 2 * CLOCK_FREQ);
    Expect(!enabled.shouldPrepareInParallel());

    StartupPolicy parallel(true, 2 * CLOCK_FREQ, true);
    Expect(parallel.shouldPrepareInParallel());
    Expect(!parallel.shouldPrefetchMediaChunk());

    StartupPolicy mediaPrefetch(true, 2 * CLOCK_FREQ, true, true);
    Expect(mediaPrefetch.shouldPrepareInParallel());
    Expect(mediaPrefetch.shouldPrefetchMediaChunk());

    StartupPolicy mediaWithoutParallel(true, 2 * CLOCK_FREQ, false, true);
    Expect(!mediaWithoutParallel.shouldPrefetchMediaChunk());

    StartupPolicy low(true, CLOCK_FREQ / 10);
    Expect(low.getMinBuffering(upstream) == CLOCK_FREQ / 2);

    StartupPolicy high(true, 20 * CLOCK_FREQ);
    Expect(high.getMinBuffering(upstream) == 6 * CLOCK_FREQ);

    StartupPolicy alreadyLow(true, 2 * CLOCK_FREQ);
    Expect(alreadyLow.getMinBuffering(CLOCK_FREQ) == CLOCK_FREQ);

    enabled.onMediaDequeued();
    enabled.onMediaDequeued();
    Expect(!enabled.isStarting());
    Expect(enabled.getMinBuffering(upstream) == upstream);
    parallel.onMediaDequeued();
    Expect(!parallel.shouldPrepareInParallel());
    mediaPrefetch.onMediaDequeued();
    Expect(!mediaPrefetch.shouldPrefetchMediaChunk());

    StartupPolicy parallelWithoutFastStart(false, 2 * CLOCK_FREQ, true);
    Expect(!parallelWithoutFastStart.shouldPrepareInParallel());
    StartupPolicy mediaWithoutFastStart(false, 2 * CLOCK_FREQ, true, true);
    Expect(!mediaWithoutFastStart.shouldPrefetchMediaChunk());

    return 0;
}
