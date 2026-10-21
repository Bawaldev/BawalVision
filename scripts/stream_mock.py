#!/usr/bin/env python3
"""
scripts/stream_mock.py - Continuous Mock Video Streamer for BawalVision

Feeds frames from an MP4 file (e.g. examples/banana/banana_sample_one.mp4)
into the Go backend ingestion API (/api/v1/camera/frame) in an infinite loop,
simulating a real-time camera feed for the C++ Vision AI engine and Grafana dashboard.
"""

import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.error

try:
    import cv2
except ImportError:
    print("[ERROR] OpenCV (cv2) is required. Install it via 'pip install opencv-python' or run in repo .venv.", file=sys.stderr)
    sys.exit(1)


def set_mock_mode(base_url: str, video_path: str, video_uuid: str = ""):
    url = f"{base_url.rstrip('/')}/api/v1/camera/mode"
    payload = {
        "mode": "mock",
        "video_source": video_path,
        "video_uuid": video_uuid
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            data = json.loads(resp.read().decode())
            print(f"[INIT] Switched backend to Mock Video mode: {data.get('message', 'OK')}")
    except Exception as e:
        print(f"[WARN] Failed to set mock mode on {url}: {e}")


def stream_video(video_path: str, base_url: str = "http://127.0.0.1:8080", fps: float = 15.0, loop: bool = True):
    if not os.path.exists(video_path):
        print(f"[ERROR] Video file not found: {video_path}", file=sys.stderr)
        sys.exit(1)

    print(f"[STREAM] Opening video: {video_path}")
    print(f"[STREAM] Ingestion target: {base_url}/api/v1/camera/frame")
    print(f"[STREAM] Target FPS: {fps} (Loop: {loop})")

    norm_video_path = video_path.replace("\\", "/")
    set_mock_mode(base_url, norm_video_path)

    frame_delay = 1.0 / max(fps, 1.0)
    frame_seq = 0
    loop_count = 0

    try:
        while True:
            cap = cv2.VideoCapture(video_path)
            if not cap.isOpened():
                print(f"[ERROR] Failed to open video source: {video_path}", file=sys.stderr)
                sys.exit(1)

            loop_count += 1
            print(f"[STREAM] Starting video playback iteration #{loop_count}...")

            while True:
                ret, frame = cap.read()
                if not ret:
                    break

                frame_seq += 1
                success, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
                if not success:
                    continue

                jpg_bytes = buf.tobytes()
                req = urllib.request.Request(
                    f"{base_url.rstrip('/')}/api/v1/camera/frame",
                    data=jpg_bytes,
                    headers={
                        "Content-Type": "image/jpeg",
                        "X-Device-ID": "MOCK_VIDEO_CAM",
                        "X-Stream-Source": "MOCK_VIDEO",
                        "X-Telemetry-Mode": "MOCK_SIMULATION",
                        "X-Telemetry-Struct": "MockTelemetryPayload",
                        "X-Mock-Video-Source": norm_video_path,
                        "X-Frame-ID": str(frame_seq),
                        "X-Is-Rewind": "0"
                    }
                )

                try:
                    with urllib.request.urlopen(req, timeout=3) as resp:
                        if resp.status != 200:
                            print(f"[WARN] Ingress returned HTTP {resp.status}")
                except Exception as e:
                    print(f"[WARN] Ingress error on frame #{frame_seq}: {e}")

                if frame_seq % 30 == 0:
                    print(f"[STREAM] Active: pushed {frame_seq} frames (Loop #{loop_count})")

                time.sleep(frame_delay)

            cap.release()

            if not loop:
                print(f"[STREAM] Reached end of video (loop=False). Finished {frame_seq} frames.")
                break

    except KeyboardInterrupt:
        print(f"\n[STREAM] Stopped by user after {frame_seq} frames.")


def main():
    parser = argparse.ArgumentParser(description="Stream mock video frames into BawalVision backend")
    parser.add_argument("video", nargs="?", default="examples/banana/banana_sample_one.mp4",
                        help="Path to MP4 video file (default: examples/banana/banana_sample_one.mp4)")
    parser.add_argument("--url", default="http://127.0.0.1:8080",
                        help="Backend gateway URL (default: http://127.0.0.1:8080)")
    parser.add_argument("--fps", type=float, default=15.0,
                        help="Streaming frames per second (default: 15.0)")
    parser.add_argument("--no-loop", action="store_true",
                        help="Do not loop the video infinitely")

    args = parser.parse_args()
    stream_video(
        video_path=args.video,
        base_url=args.url,
        fps=args.fps,
        loop=not args.no_loop
    )


if __name__ == "__main__":
    main()

