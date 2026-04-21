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

The tests exercise RTSP control-plane behavior. Integration playback can be checked with VLC or ffplay.
