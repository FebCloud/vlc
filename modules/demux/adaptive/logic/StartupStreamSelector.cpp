/*****************************************************************************
 * StartupStreamSelector.cpp
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "StartupStreamSelector.hpp"

#include <limits>

using namespace adaptive::logic;

StartupDownloadLaneSelector::StartupDownloadLaneSelector() :
    enabled(false), normalSequence(0), highPrioritySequence(0)
{
}

void StartupDownloadLaneSelector::setEnabled(bool value)
{
    if(value)
    {
        normalSequence.store(0, std::memory_order_relaxed);
        highPrioritySequence.store(0, std::memory_order_relaxed);
    }
    enabled.store(value, std::memory_order_release);
}

bool StartupDownloadLaneSelector::useSecondary(bool highPriority)
{
    if(!enabled.load(std::memory_order_acquire))
        return false;

    std::atomic<unsigned> &sequence = highPriority ? highPrioritySequence
                                                    : normalSequence;
    return sequence.fetch_add(1, std::memory_order_relaxed) % 2 == 1;
}

StartupStreamKind adaptive::logic::mergeStartupStreamKind(StartupStreamKind current,
                                                           StartupStreamKind next)
{
    if(next == StartupStreamKind::Unknown)
        return current;
    if(current == StartupStreamKind::Unknown)
        return next;
    if(current == next)
        return current;
    return StartupStreamKind::Multiplexed;
}

StartupStreamSelection adaptive::logic::selectStartupStreams(
    const std::vector<StartupStreamCandidate> &candidates)
{
    const size_t missing = std::numeric_limits<size_t>::max();
    StartupStreamSelection selection = { missing, missing };

    for(size_t i = 0; i < candidates.size(); ++i)
    {
        const StartupStreamCandidate &candidate = candidates[i];
        if(!candidate.valid || !candidate.selected || candidate.disabled)
            continue;

        if(candidate.kind == StartupStreamKind::Audio && selection.audio == missing)
            selection.audio = i;
        else if(candidate.kind == StartupStreamKind::Video && selection.video == missing)
            selection.video = i;

        if(selection.audio != missing && selection.video != missing)
            break;
    }

    return selection;
}
