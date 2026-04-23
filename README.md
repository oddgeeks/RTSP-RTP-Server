# Minimal RTSP/RTP Video Server

A small C++20 console RTSP server for the Network Optix takehome. It handles `OPTIONS`, `DESCRIBE`, `SETUP`, and `PLAY`, then uses FFmpeg to transcode video to H.264 inside RTP MPEG-TS so VLC can play URLs like:

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

## Test Process

There are two useful levels of testing: automated RTSP control-plane tests and a manual media-playback smoke test.

### 1. Automated Tests

Build the server and run the Python integration tests:

```bash
make test
```

`make test` does the following:

- Builds `./rtsp-server` if needed.
- Starts the server on a temporary localhost TCP port.
- Creates a temporary media root containing `sample.mp4`.
- Sends real RTSP requests over a socket.
- Verifies `OPTIONS` returns the supported method list.
- Verifies `DESCRIBE` returns SDP for an existing file.
- Verifies missing media returns `404 Not Found`.
- Verifies track-level `SETUP` accepts `client_port=<rtp>-<rtcp>` and returns a session plus server RTP ports.

Expected result:

```text
Ran 4 tests

OK
```

You can also run the tests directly:

```bash
python3 -m unittest discover -s tests -p '*_test.py'
```

### 2. Generate A Local Test Video

For manual playback testing, create a small sample file under a media directory:

```bash
mkdir -p media
ffmpeg -f lavfi -i testsrc=size=640x360:rate=30 -t 10 -pix_fmt yuv420p media/sample.mp4
```

Start the server with that directory as the media root:

```bash
./rtsp-server ./media 127.0.0.1:8554
```

The URL path maps to a file inside the media root:

```text
rtsp://127.0.0.1:8554/sample.mp4 -> ./media/sample.mp4
```

### 3. Manual Playback With VLC

Open the stream in VLC:

```bash
vlc rtsp://127.0.0.1:8554/sample.mp4
```

Or from the VLC UI:

- Open `Media` -> `Open Network Stream`.
- Enter `rtsp://127.0.0.1:8554/sample.mp4`.
- Press `Play`.

The video should play and loop continuously.

VLC may show the RTSP stream length as `0` because the server exposes the media as a live RTP stream. That is expected; successful playback is indicated by video frames appearing, not by a finite duration in the VLC timeline.

For verbose VLC diagnostics without the GUI, run:

```bash
cvlc -vvv --intf dummy --play-and-exit --run-time=3 rtsp://127.0.0.1:8554/sample.mp4
```

### 4. Headless Playback Smoke Test

If you do not want to use the VLC UI, `ffprobe` can verify that the RTSP/RTP stream is readable:

```bash
ffprobe -v error \
  -rtsp_transport udp \
  -i rtsp://127.0.0.1:8554/sample.mp4 \
  -select_streams v:0 \
  -show_entries stream=codec_name,width,height \
  -of compact=p=0:nk=1
```

Expected output is similar to:

```text
h264|640|360
```

### 5. Useful Failure Checks

- If binding to port `554` fails, use a higher port such as `8554`; ports below `1024` often require elevated privileges.
- If VLC shows a `0` length, that is normal for this live RTSP stream. If video does not appear, run the `ffprobe` smoke test above to separate VLC UI behavior from RTP delivery.
- If VLC cannot connect, confirm the server is running and the URL filename exists under the directory passed to `./rtsp-server`.
- If playback starts but no video appears, check that `ffmpeg` is installed and available on `PATH`.
- If testing from another machine, bind to an externally reachable address such as `0.0.0.0:8554` and use the server machine's IP in the RTSP URL.

## Docker

Build and run the optional container image:

```bash
docker build -t minimal-rtsp-server .
docker run --rm -p 8554:8554 -v "$PWD/media:/media:ro" minimal-rtsp-server
```

Then open `rtsp://127.0.0.1:8554/<filename>` in VLC.

## Notes

- RTSP control is handled asynchronously with `poll(2)` so multiple clients can connect at once.
- RTP MPEG-TS media packetization is delegated to one FFmpeg child process per playing client.
- `DESCRIBE`/`SETUP` validate paths under the configured root to avoid directory traversal.
- Playback loops forever using FFmpeg's `-stream_loop -1`.
