/*****************************************************************************
 * StartupChunks.cpp
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "../StartupChunks.hpp"
#include "../http/Chunk.h"
#include "test.hpp"

#include <vector>

using namespace adaptive;
using namespace adaptive::http;

class StartupTestChunk : public ChunkInterface
{
public:
    StartupTestChunk(ChunkType type_, unsigned *destroyed_)
        : type(type_), destroyed(destroyed_) {}
    ~StartupTestChunk() override { ++*destroyed; }

    ChunkType getChunkType() const override { return type; }
    const std::string &getContentType() const override { return contentType; }
    RequestStatus getRequestStatus() const override { return RequestStatus::Success; }
    block_t *readBlock() override { return nullptr; }
    block_t *read(size_t) override { return nullptr; }
    bool hasMoreData() const override { return false; }
    size_t getBytesRead() const override { return 0; }

private:
    ChunkType type;
    unsigned *destroyed;
    std::string contentType;
};

class StartupChunkProvider
{
public:
    explicit StartupChunkProvider(const std::vector<ChunkInterface *> &chunks_)
        : chunks(chunks_), position(0) {}

    ChunkInterface *next()
    {
        return position < chunks.size() ? chunks[position++] : nullptr;
    }

    size_t calls() const { return position; }

private:
    std::vector<ChunkInterface *> chunks;
    size_t position;
};

int StartupChunks_test()
{
    unsigned destroyed = 0;
    StartupChunkProvider provider({
        new StartupTestChunk(ChunkType::Init, &destroyed),
        new StartupTestChunk(ChunkType::Segment, &destroyed),
        new StartupTestChunk(ChunkType::Segment, &destroyed),
    });

    StartupChunks chunks;
    const size_t prepared = chunks.prepare(
        [&provider]() { return provider.next(); }, true);
    Expect(prepared == 2);
    Expect(provider.calls() == 2);

    ChunkInterface *init = chunks.take();
    ChunkInterface *media = chunks.take();
    Expect(init != nullptr && init->getChunkType() == ChunkType::Init);
    Expect(media != nullptr && media->getChunkType() == ChunkType::Segment);
    Expect(chunks.take() == nullptr);
    delete init;
    delete media;
    Expect(destroyed == 2);

    delete provider.next();
    Expect(destroyed == 3);

    {
        StartupChunkProvider withIndex({
            new StartupTestChunk(ChunkType::Init, &destroyed),
            new StartupTestChunk(ChunkType::Index, &destroyed),
            new StartupTestChunk(ChunkType::Segment, &destroyed),
        });
        StartupChunks owned;
        Expect(owned.prepare([&withIndex]() { return withIndex.next(); }, true) == 3);
        Expect(withIndex.calls() == 3);
    }
    Expect(destroyed == 6);

    StartupChunkProvider oneOnly({
        new StartupTestChunk(ChunkType::Init, &destroyed),
        new StartupTestChunk(ChunkType::Segment, &destroyed),
    });
    {
        StartupChunks stageTwo;
        Expect(stageTwo.prepare([&oneOnly]() { return oneOnly.next(); }, false) == 1);
        Expect(oneOnly.calls() == 1);
    }
    delete oneOnly.next();
    Expect(destroyed == 8);

    return 0;
}
