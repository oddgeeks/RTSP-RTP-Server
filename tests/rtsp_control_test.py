import os
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "rtsp-server"


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class RtspControlPlaneTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.media_dir = Path(self.tmp.name)
        (self.media_dir / "sample.mp4").write_bytes(b"not a real mp4; control tests only")
        self.port = free_port()
        self.proc = subprocess.Popen(
            [str(SERVER), str(self.media_dir), f"127.0.0.1:{self.port}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            text=True,
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.proc.poll() is not None:
                self.fail("server exited early")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.05)
        self.fail("server did not start listening")

    def tearDown(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
        self.tmp.cleanup()

    def request(self, request_text):
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as sock:
            sock.sendall(request_text.encode("ascii"))
            sock.settimeout(2)
            data = sock.recv(8192)
        return data.decode("ascii", errors="replace")

    def test_options_lists_required_methods(self):
        response = self.request(
            "OPTIONS rtsp://127.0.0.1:{}/sample.mp4 RTSP/1.0\r\n"
            "CSeq: 1\r\n\r\n".format(self.port)
        )
        self.assertIn("RTSP/1.0 200 OK", response)
        self.assertIn("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN", response)
        self.assertIn("CSeq: 1", response)

    def test_describe_returns_sdp_for_existing_file(self):
        response = self.request(
            "DESCRIBE rtsp://127.0.0.1:{}/sample.mp4 RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Accept: application/sdp\r\n\r\n".format(self.port)
        )
        self.assertIn("RTSP/1.0 200 OK", response)
        self.assertIn("Content-Type: application/sdp", response)
        self.assertIn("c=IN IP4 127.0.0.1", response)
        self.assertIn("a=range:npt=0-", response)
        self.assertIn("a=sendonly", response)
        self.assertIn("m=video 0 RTP/AVP 33", response)
        self.assertIn("a=rtpmap:33 MP2T/90000", response)
        self.assertIn("a=control:trackID=0", response)

    def test_missing_file_returns_404(self):
        response = self.request(
            "DESCRIBE rtsp://127.0.0.1:{}/missing.mp4 RTSP/1.0\r\n"
            "CSeq: 3\r\n\r\n".format(self.port)
        )
        self.assertIn("RTSP/1.0 404 Not Found", response)

    def test_setup_accepts_track_uri_and_client_ports(self):
        response = self.request(
            "SETUP rtsp://127.0.0.1:{}/sample.mp4/trackID=0 RTSP/1.0\r\n"
            "CSeq: 4\r\n"
            "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n\r\n".format(self.port)
        )
        self.assertIn("RTSP/1.0 200 OK", response)
        self.assertIn("Session:", response)
        self.assertIn("client_port=5000-5001", response)
        self.assertIn("server_port=", response)

    def test_play_accepts_vlc_stream_uri_after_aggregate_setup(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as sock:
            sock.sendall(
                (
                    "SETUP rtsp://127.0.0.1:{}/sample.mp4 RTSP/1.0\r\n"
                    "CSeq: 5\r\n"
                    "Transport: RTP/AVP;unicast;client_port=5002-5003\r\n\r\n"
                ).format(self.port).encode("ascii")
            )
            setup = sock.recv(8192).decode("ascii", errors="replace")
            self.assertIn("RTSP/1.0 200 OK", setup)
            self.assertIn("Session:", setup)

            sock.sendall(
                (
                    "PLAY rtsp://127.0.0.1:{}/stream=0 RTSP/1.0\r\n"
                    "CSeq: 6\r\n\r\n"
                ).format(self.port).encode("ascii")
            )
            play = sock.recv(8192).decode("ascii", errors="replace")
            self.assertIn("RTSP/1.0 200 OK", play)
            self.assertIn("RTP-Info: url=rtsp://127.0.0.1:{}/stream=0".format(self.port), play)


if __name__ == "__main__":
    unittest.main()
