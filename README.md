# Minimal RTSP/RTP Video Server

A small C++20 console RTSP server for the Network Optix takehome. It handles `OPTIONS`, `DESCRIBE`, `SETUP`, and `PLAY`, then uses FFmpeg to packetize/transcode video into RTP so VLC can play URLs like:

```bash
vlc rtsp://127.0.0.1:8554/sample.mp4
```

## Build

Ubuntu packages needed:

```bash
sudo apt-get update
sudo apt-get install -y build-essential make ffmpeg python3 vlc
```

Then:

```bash
make
```

## Run

```bash
./rtsp-server [directory] [host:port]
```

Defaults:

- `directory`: current working directory
- `host:port`: `localhost:554`

Example:

```bash
./rtsp-server ./media 127.0.0.1:8554
```

## Tests

```bash
make test
```

The Python integration tests start the server and exercise RTSP control-plane behavior, including `404` handling and track-level `SETUP`. Full playback can be checked with VLC or ffplay.

## Docker

Build and run the optional container image:

```bash
docker build -t minimal-rtsp-server .
docker run --rm -p 8554:8554 -v "$PWD/media:/media:ro" minimal-rtsp-server
```

Then open `rtsp://127.0.0.1:8554/<filename>` in VLC.

## Notes

- RTSP control is handled asynchronously with `poll(2)` so multiple clients can connect at once.
- RTP media packetization is delegated to one FFmpeg child process per playing client.
- `DESCRIBE`/`SETUP` validate paths under the configured root to avoid directory traversal.
- Playback loops forever using FFmpeg's `-stream_loop -1`.
