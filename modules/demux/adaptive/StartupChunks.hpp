/*****************************************************************************
 * StartupChunks.hpp
 *****************************************************************************/
#ifndef STARTUPCHUNKS_HPP
#define STARTUPCHUNKS_HPP

#include "http/Chunk.h"

#include <deque>
#include <functional>

namespace adaptive
{
    class StartupChunks
    {
    public:
        StartupChunks() = default;
        ~StartupChunks();

        size_t prepare(const std::function<http::ChunkInterface *()> &, bool);
        http::ChunkInterface *take();
        void clear();

    private:
        std::deque<http::ChunkInterface *> chunks;
    };
}

#endif
