/*****************************************************************************
 * ConnectionReuse.cpp
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "../../http/Chunk.h"
#include "../../http/HTTPConnection.hpp"
#include "../../http/HTTPConnectionManager.h"
#include "../test.hpp"

#include <vlc_block.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace adaptive;
using namespace adaptive::http;

class ReusableTestConnection : public AbstractConnection
{
public:
    ReusableTestConnection() : AbstractConnection(nullptr), offset(0), body(4, 0x2a) {}

    bool canReuse(const ConnectionParams &requested) const override
    {
        return available && params.getHostname() == requested.getHostname() &&
               params.getScheme() == requested.getScheme() &&
               params.getPort() == requested.getPort();
    }

    RequestStatus request(const std::string &, const BytesRange &) override
    {
        offset = 0;
        bytesRead = 0;
        contentLength = body.size();
        contentType = "application/octet-stream";
        return RequestStatus::Success;
    }

    ssize_t read(void *buffer, size_t length) override
    {
        const size_t remaining = body.size() - offset;
        const size_t copied = std::min(length, remaining);
        if(copied)
            std::memcpy(buffer, &body[offset], copied);
        offset += copied;
        bytesRead += copied;
        return copied;
    }

    void setUsed(bool used) override
    {
        available = !used;
    }

private:
    size_t offset;
    std::vector<uint8_t> body;
};

class CountingConnectionFactory : public AbstractConnectionFactory
{
public:
    explicit CountingConnectionFactory(unsigned *created_) : created(created_) {}

    AbstractConnection *createConnection(vlc_object_t *, const ConnectionParams &) override
    {
        ++*created;
        return new ReusableTestConnection();
    }

private:
    unsigned *created;
};

static void readBufferedSource(AbstractChunkSource *source)
{
    block_t *block = source->readBlock();
    Expect(block != nullptr);
    Expect(block->i_buffer == 4);
    block_Release(block);

    /* Drain the zero-length EOF marker. The downloader releases the HTTP
     * connection before publishing this marker, so the next source can reuse
     * it deterministically instead of depending on thread scheduling. */
    block = source->readBlock();
    Expect(block != nullptr);
    Expect(block->i_buffer == 0);
    block_Release(block);
}

int ConnectionReuse_test()
{
    unsigned created = 0;
    HTTPConnectionManager manager(nullptr);
    manager.addFactory(new CountingConnectionFactory(&created));

    AbstractChunkSource *first = manager.makeSource(
        "https://example.test/first.m4s", ID(), ChunkType::Segment, BytesRange());
    Expect(first != nullptr);
    manager.start(first);
    readBufferedSource(first);

    AbstractChunkSource *second = manager.makeSource(
        "https://example.test/second.m4s", ID(), ChunkType::Segment, BytesRange());
    Expect(second != nullptr);
    manager.start(second);
    readBufferedSource(second);

    Expect(created == 1);

    manager.recycleSource(first);
    manager.recycleSource(second);
    return 0;
}
