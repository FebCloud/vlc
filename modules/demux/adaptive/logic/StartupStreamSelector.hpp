/*****************************************************************************
 * StartupStreamSelector.hpp
 *****************************************************************************/
#ifndef STARTUPSTREAMSELECTOR_HPP
#define STARTUPSTREAMSELECTOR_HPP

#include <cstddef>
#include <atomic>
#include <vector>

namespace adaptive
{
    namespace logic
    {
        enum class StartupStreamKind
        {
            Unknown,
            Audio,
            Video,
            Subtitle,
            Multiplexed,
        };

        struct StartupStreamCandidate
        {
            StartupStreamKind kind;
            bool valid;
            bool selected;
            bool disabled;
        };

        struct StartupStreamSelection
        {
            size_t audio;
            size_t video;
        };

        class StartupDownloadLaneSelector
        {
            public:
                StartupDownloadLaneSelector();
                void setEnabled(bool);
                bool useSecondary(bool highPriority);

            private:
                std::atomic<bool> enabled;
                std::atomic<unsigned> normalSequence;
                std::atomic<unsigned> highPrioritySequence;
        };

        StartupStreamKind mergeStartupStreamKind(StartupStreamKind,
                                                 StartupStreamKind);
        StartupStreamSelection selectStartupStreams(
            const std::vector<StartupStreamCandidate> &);
    }
}

#endif
