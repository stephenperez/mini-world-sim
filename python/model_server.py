#!/usr/bin/env python
"""Small MiniWorldSim neural-rendering API server.

The current model contract is intentionally simple:

    POST /render
        minimap_semantic_b64: base64 uint8 CHW semantic minimap

    response
        rgb_b64: base64 uint8 HWC RGB generated frame

This matches the starter map-only renderer trained by train_renderer.py.
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import torch

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from train_renderer import MiniUNet, choose_amp_dtype  # noqa: E402


class Renderer:
    def __init__(self, args: argparse.Namespace):
        self.device = torch.device("cuda" if torch.cuda.is_available() and not args.cpu else "cpu")
        self.amp_dtype = choose_amp_dtype(self.device, args.amp)
        self.channels_last = args.channels_last and self.device.type == "cuda"
        self.lock = threading.Lock()

        checkpoint_path = Path(args.checkpoint).resolve()
        checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
        config = checkpoint.get("config", {})
        base_channels = int(config.get("base_channels", args.base_channels))

        self.model = MiniUNet(in_channels=6, out_channels=3, base_channels=base_channels)
        self.model.load_state_dict(checkpoint["model"])
        self.model.to(self.device)
        if self.channels_last:
            self.model = self.model.to(memory_format=torch.channels_last)
        self.model.eval()
        if args.compile:
            self.model = torch.compile(self.model)

        self.checkpoint_path = checkpoint_path
        self.base_channels = base_channels
        self.frame_width = int(args.frame_width)
        self.frame_height = int(args.frame_height)

        for _ in range(max(0, args.warmup)):
            dummy = torch.zeros(1, 6, self.frame_height, self.frame_width, device=self.device)
            if self.channels_last:
                dummy = dummy.contiguous(memory_format=torch.channels_last)
            self._infer(dummy)

    def health(self) -> dict[str, Any]:
        return {
            "ok": True,
            "checkpoint": str(self.checkpoint_path),
            "device": str(self.device),
            "amp_dtype": str(self.amp_dtype),
            "torch": torch.__version__,
            "cuda": torch.version.cuda,
            "gpu": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            "base_channels": self.base_channels,
            "frame_width": self.frame_width,
            "frame_height": self.frame_height,
        }

    def render_payload(self, payload: dict[str, Any]) -> dict[str, Any]:
        started = time.perf_counter()
        minimap_size = int(payload.get("minimap_size", self.frame_width))
        semantic_channels = int(payload.get("semantic_channels", 6))
        semantic_b64 = payload.get("minimap_semantic_b64")
        if not isinstance(semantic_b64, str) or not semantic_b64:
            raise ValueError("Request must include non-empty minimap_semantic_b64.")
        if semantic_channels != 6:
            raise ValueError(f"Expected 6 semantic channels, got {semantic_channels}.")
        if minimap_size != self.frame_width or minimap_size != self.frame_height:
            raise ValueError(
                f"Expected {self.frame_width}x{self.frame_height} minimap, got {minimap_size}x{minimap_size}."
            )

        raw = base64.b64decode(semantic_b64, validate=True)
        expected_bytes = semantic_channels * minimap_size * minimap_size
        if len(raw) != expected_bytes:
            raise ValueError(f"Expected {expected_bytes} semantic bytes, got {len(raw)}.")

        x = torch.frombuffer(bytearray(raw), dtype=torch.uint8)
        x = x.reshape(semantic_channels, minimap_size, minimap_size).float().div_(255.0).unsqueeze(0)
        x = x.to(self.device, non_blocking=True)
        if self.channels_last:
            x = x.contiguous(memory_format=torch.channels_last)

        with self.lock:
            y = self._infer(x)

        rgb = (y.squeeze(0).detach().cpu().clamp(0, 1) * 255.0).byte()
        rgb_hwc = rgb.permute(1, 2, 0).contiguous().numpy().tobytes()
        elapsed_ms = (time.perf_counter() - started) * 1000.0

        return {
            "ok": True,
            "request_id": payload.get("request_id"),
            "format": "rgb_u8",
            "layout": "HWC",
            "width": self.frame_width,
            "height": self.frame_height,
            "channels": 3,
            "rgb_b64": base64.b64encode(rgb_hwc).decode("ascii"),
            "inference_ms": elapsed_ms,
        }

    def _infer(self, x: torch.Tensor) -> torch.Tensor:
        with torch.no_grad():
            if self.amp_dtype is None:
                return self.model(x)
            with torch.autocast(device_type=self.device.type, dtype=self.amp_dtype):
                return self.model(x)


def make_handler(renderer: Renderer, quiet: bool) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "MiniWorldSimModelServer/0.1"

        def log_message(self, fmt: str, *args: Any) -> None:
            if not quiet:
                super().log_message(fmt, *args)

        def do_GET(self) -> None:
            if self.path.rstrip("/") == "/health":
                self._send_json(200, renderer.health())
                return
            self._send_json(404, {"ok": False, "error": "Not found."})

        def do_POST(self) -> None:
            if self.path.rstrip("/") != "/render":
                self._send_json(404, {"ok": False, "error": "Not found."})
                return

            try:
                content_length = int(self.headers.get("Content-Length", "0"))
                payload_bytes = self.rfile.read(content_length)
                payload = json.loads(payload_bytes.decode("utf-8"))
                response = renderer.render_payload(payload)
            except Exception as exc:
                self._send_json(400, {"ok": False, "error": str(exc)})
                return

            self._send_json(200, response)

        def _send_json(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return Handler


def run_self_test(renderer: Renderer, shard_path: Path, out_path: Path) -> None:
    from PIL import Image

    payload = torch.load(shard_path, map_location="cpu", weights_only=False)
    sample = payload["samples"][0]
    semantic = sample["minimap_semantic_t1"].contiguous().byte().numpy().tobytes()
    response = renderer.render_payload(
        {
            "request_id": "self_test",
            "minimap_size": int(sample["minimap_semantic_t1"].shape[-1]),
            "semantic_channels": int(sample["minimap_semantic_t1"].shape[0]),
            "minimap_semantic_b64": base64.b64encode(semantic).decode("ascii"),
        }
    )
    rgb = base64.b64decode(response["rgb_b64"])
    image = Image.frombytes("RGB", (response["width"], response["height"]), rgb)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(out_path)
    print(json.dumps({"ok": True, "self_test_out": str(out_path), "response": {k: v for k, v in response.items() if k != "rgb_b64"}}, indent=2))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", default="Saved/Training/map_only_0002/best.pt")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--frame-width", type=int, default=128)
    parser.add_argument("--frame-height", type=int, default=128)
    parser.add_argument("--base-channels", type=int, default=48)
    parser.add_argument("--amp", choices=["auto", "bf16", "fp16", "none"], default="auto")
    parser.add_argument("--channels-last", action="store_true")
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--log-file", default="")
    parser.add_argument("--self-test-shard", default="")
    parser.add_argument("--self-test-out", default="Saved/Training/model_server_self_test.png")
    return parser.parse_args()


def run(args: argparse.Namespace) -> None:
    renderer = Renderer(args)

    if args.self_test_shard:
        run_self_test(renderer, Path(args.self_test_shard), Path(args.self_test_out))
        return

    server = ThreadingHTTPServer((args.host, args.port), make_handler(renderer, args.quiet))
    if not args.quiet:
        print(json.dumps({"ok": True, "listening": f"http://{args.host}:{args.port}", "health": renderer.health()}, indent=2))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def main() -> None:
    args = parse_args()
    try:
        run(args)
    except Exception:
        if args.log_file:
            log_path = Path(args.log_file)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(traceback.format_exc(), encoding="utf-8")
        raise


if __name__ == "__main__":
    main()
