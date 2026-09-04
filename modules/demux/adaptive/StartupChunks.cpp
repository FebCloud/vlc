/*****************************************************************************
 * StartupChunks.cpp
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "StartupChunks.hpp"

using namespace adaptive;
using namespace adaptive::http;

StartupChunks::~StartupChunks()
{
    clear();
}

size_t StartupChunks::prepare(const std::function<ChunkInterface *()> &next,
                              bool throughMedia)
{
    static const size_t maxStartupChunks = 3;
    size_t prepared = 0;

    while(prepared < maxStartupChunks)
    {
        ChunkInterface *chunk = next();
        if(!chunk)
            break;

        chunks.push_back(chunk);
        ++prepared;
        if(!throughMedia || chunk->getChunkType() == ChunkType::Segment)
            break;
    }

    return prepared;
}

ChunkInterface *StartupChunks::take()
{
    if(chunks.empty())
        return nullptr;

    ChunkInterface *chunk = chunks.front();
    chunks.pop_front();
    return chunk;
}

void StartupChunks::clear()
{
    while(!chunks.empty())
    {
        delete chunks.front();
        chunks.pop_front();
    }
}
