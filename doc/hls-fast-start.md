# Adaptive HLS fast start

This branch carries an opt-in VLC 4 port of the VLC 3 adaptive HLS startup
optimization. It keeps VLC's normal adaptation, representation, codec, audio
track, language, and ES selection decisions authoritative.

## Runtime options

All options are disabled by default:

- `--adaptive-fast-start` temporarily lowers only the initial minimum
  buffering threshold. Normal buffering is restored after the first advancing
  media dequeue.
- `--adaptive-startup-buffer=<ms>` selects the temporary minimum and is
  clamped to 500-6000 ms. The default is 2000 ms.
- `--adaptive-startup-parallel` prepares at most the first already-selected
  audio stream and first selected video stream concurrently.
- `--adaptive-startup-media` extends parallel preparation through exactly the
  first media segment of each selected stream.

Example:

```sh
vlc \
  --adaptive-fast-start \
  --adaptive-startup-buffer=2000 \
  --adaptive-startup-parallel \
  --adaptive-startup-media \
  'https://example.test/master.m3u8'
```

Omitting all four options retains upstream VLC 4 behavior. Omitting only
`--adaptive-startup-media` keeps selected-stream playlist/init preparation but
does not prefetch the first media segment.

## Implementation boundary

- `StartupPolicy` owns the one-way startup-to-steady-state transition.
- `StartupStreamSelector` limits speculation to VLC-selected audio/video
  streams and assigns temporary secondary downloader lanes.
- `StartupChunks` preserves normal init/index/media chunk ordering without
  starting a separate demuxer.
- `HTTPChunkBufferedSource` releases completed HTTP connections early so a
  following startup request can reuse the connection and TLS session.
- Secondary downloader lanes stop accepting new work after the first
  advancing media dequeue and are destroyed with the connection manager.

Progressive child-playlist probing from the VLC 3 experiment is intentionally
not included because its recorded A/B median was slower than Stage 3.

## Verification

Build and run the adaptive test target:

```sh
make -C build/modules adaptive_test
build/modules/adaptive_test
```

Then compare an unmodified build and this build using alternating runs on the
same network path. Confirm time to first picture, selected audio/video streams,
hardware decoding, request count, and TLS connection count. Also verify a run
without the four options behaves like upstream VLC 4.
