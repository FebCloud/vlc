/*****************************************************************************
 * StartupStreamSelector.cpp
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "../../logic/StartupStreamSelector.hpp"
#include "../test.hpp"

#include <limits>
#include <vector>

using namespace adaptive::logic;

int StartupStreamSelector_test()
{
    const size_t missing = std::numeric_limits<size_t>::max();

    Expect(mergeStartupStreamKind(StartupStreamKind::Unknown,
                                  StartupStreamKind::Audio) == StartupStreamKind::Audio);
    Expect(mergeStartupStreamKind(StartupStreamKind::Audio,
                                  StartupStreamKind::Audio) == StartupStreamKind::Audio);
    Expect(mergeStartupStreamKind(StartupStreamKind::Audio,
                                  StartupStreamKind::Video) == StartupStreamKind::Multiplexed);
    Expect(mergeStartupStreamKind(StartupStreamKind::Multiplexed,
                                  StartupStreamKind::Audio) == StartupStreamKind::Multiplexed);

    StartupDownloadLaneSelector lanes;
    Expect(!lanes.useSecondary(false));
    lanes.setEnabled(true);
    Expect(!lanes.useSecondary(false));
    Expect(lanes.useSecondary(false));
    Expect(!lanes.useSecondary(false));
    Expect(!lanes.useSecondary(true));
    Expect(lanes.useSecondary(true));
    lanes.setEnabled(false);
    Expect(!lanes.useSecondary(false));
    Expect(!lanes.useSecondary(true));

    std::vector<StartupStreamCandidate> candidates = {
        { StartupStreamKind::Audio, true, false, false },
        { StartupStreamKind::Audio, true, true, false },
        { StartupStreamKind::Audio, true, true, false },
        { StartupStreamKind::Subtitle, true, true, false },
        { StartupStreamKind::Video, true, true, false },
        { StartupStreamKind::Video, true, true, false },
    };

    StartupStreamSelection selected = selectStartupStreams(candidates);
    Expect(selected.audio == 1);
    Expect(selected.video == 4);

    candidates = {
        { StartupStreamKind::Audio, false, true, false },
        { StartupStreamKind::Audio, true, true, true },
        { StartupStreamKind::Unknown, true, true, false },
        { StartupStreamKind::Multiplexed, true, true, false },
        { StartupStreamKind::Video, true, false, false },
    };
    selected = selectStartupStreams(candidates);
    Expect(selected.audio == missing);
    Expect(selected.video == missing);

    candidates = {
        { StartupStreamKind::Video, true, true, false },
        { StartupStreamKind::Audio, true, true, false },
    };
    selected = selectStartupStreams(candidates);
    Expect(selected.audio == 1);
    Expect(selected.video == 0);

    return 0;
}
