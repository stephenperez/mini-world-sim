#!/usr/bin/env python
"""Pack MiniWorldSim capture directories into PyTorch shard files."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any


def require_dependencies():
    try:
        import torch  # noqa: F401
    except ImportError as exc:
        raise SystemExit(
            "pack_captures.py requires PyTorch. Install/use a Python environment with torch available."
        ) from exc

    try:
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        raise SystemExit(
            "pack_captures.py requires Pillow for PNG loading. Install/use a Python environment with PIL available."
        ) from exc


def load_rgb_chw(path: Path):
    import torch
    from PIL import Image

    with Image.open(path) as image:
        rgb = image.convert("RGB")
        data = torch.frombuffer(bytearray(rgb.tobytes()), dtype=torch.uint8)
        return data.view(rgb.height, rgb.width, 3).permute(2, 0, 1).contiguous()


def load_semantic(path: Path, metadata: dict[str, Any], key: str):
    import torch

    semantic_info = metadata.get("semantic_minimap", {})
    shape = semantic_info.get("shape", [6, 128, 128])
    expected_count = int(shape[0]) * int(shape[1]) * int(shape[2])
    raw = path.read_bytes()
    if len(raw) != expected_count:
        raise ValueError(f"{path} has {len(raw)} bytes; expected {expected_count} for shape {shape}")

    data = torch.frombuffer(bytearray(raw), dtype=torch.uint8)
    return data.view(int(shape[0]), int(shape[1]), int(shape[2])).contiguous()


def tensor_from_numbers(values: list[float]):
    import torch

    return torch.tensor(values, dtype=torch.float32)


def load_sample(sample_dir: Path) -> dict[str, Any]:
    metadata_path = sample_dir / "metadata.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    artifacts = metadata.get("artifacts", {})

    sample: dict[str, Any] = {
        "sample_id": metadata.get("sample_id", sample_dir.name),
        "source_dir": str(sample_dir),
        "metadata": metadata,
    }

    for key in ("rgb_t", "rgb_t1", "minimap_rgb_t", "minimap_rgb_t1"):
        artifact = artifacts.get(key)
        if artifact:
            sample[key] = load_rgb_chw(sample_dir / artifact)

    for key in ("minimap_semantic_t", "minimap_semantic_t1"):
        artifact = artifacts.get(key)
        if artifact:
            sample[key] = load_semantic(sample_dir / artifact, metadata, key)

    action = metadata.get("action", {})
    pose_t = metadata.get("pose_t", {})
    pose_t1 = metadata.get("pose_t1", {})
    motion = metadata.get("actual_motion", {})

    sample["action"] = tensor_from_numbers(
        [
            float(action.get("forward", 0.0)),
            float(action.get("strafe", 0.0)),
            float(action.get("turn", 0.0)),
            float(action.get("duration_seconds", 0.0)),
        ]
    )
    sample["pose_t"] = tensor_from_numbers(
        [
            float(pose_t.get("x", 0.0)),
            float(pose_t.get("y", 0.0)),
            float(pose_t.get("yaw_degrees", 0.0)),
        ]
    )
    sample["pose_t1"] = tensor_from_numbers(
        [
            float(pose_t1.get("x", 0.0)),
            float(pose_t1.get("y", 0.0)),
            float(pose_t1.get("yaw_degrees", 0.0)),
        ]
    )
    sample["actual_motion"] = tensor_from_numbers(
        [
            float(motion.get("delta_x", 0.0)),
            float(motion.get("delta_y", 0.0)),
            float(motion.get("delta_yaw_degrees", 0.0)),
            1.0 if bool(motion.get("collision", False)) else 0.0,
        ]
    )

    return sample


def discover_sample_dirs(input_dir: Path) -> list[Path]:
    if (input_dir / "metadata.json").is_file():
        return [input_dir]

    sample_dirs = [
        child
        for child in input_dir.iterdir()
        if child.is_dir() and (child / "metadata.json").is_file()
    ]
    return sorted(sample_dirs, key=lambda path: path.name)


def shard_output_paths(output: Path, shard_count: int) -> list[Path]:
    if shard_count <= 1:
        return [output]

    if output.suffix:
        stem = output.with_suffix("")
        suffix = output.suffix
        return [stem.parent / f"{stem.name}-{index:06d}{suffix}" for index in range(shard_count)]

    return [output / f"shard-{index:06d}.pt" for index in range(shard_count)]


def save_shard(path: Path, samples: list[dict[str, Any]], source_dir: Path, shard_index: int, shard_count: int, overwrite: bool):
    import torch

    if path.exists() and not overwrite:
        raise FileExistsError(f"Output already exists: {path}")

    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    payload = {
        "format": "mini_world_sim_capture_shard",
        "format_version": 1,
        "source_dir": str(source_dir),
        "shard_index": shard_index,
        "shard_count": shard_count,
        "num_samples": len(samples),
        "samples": samples,
    }
    torch.save(payload, tmp_path)
    tmp_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True, type=Path, help="Directory containing sample subdirectories.")
    parser.add_argument("--output", required=True, type=Path, help="Output .pt file, or output stem when sharding.")
    parser.add_argument("--samples-per-shard", type=int, default=0, help="0 means write all samples to one .pt file.")
    parser.add_argument("--delete-input", action="store_true", help="Delete --input-dir after all shards are written.")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing output shard files.")
    args = parser.parse_args()

    require_dependencies()

    input_dir = args.input_dir.resolve()
    output = args.output.resolve()
    if not input_dir.exists():
        raise SystemExit(f"Input directory does not exist: {input_dir}")

    sample_dirs = discover_sample_dirs(input_dir)
    if not sample_dirs:
        raise SystemExit(f"No sample directories with metadata.json found under {input_dir}")

    samples = [load_sample(sample_dir) for sample_dir in sample_dirs]
    samples_per_shard = max(args.samples_per_shard, 0)
    if samples_per_shard <= 0:
        chunks = [samples]
    else:
        chunks = [samples[index : index + samples_per_shard] for index in range(0, len(samples), samples_per_shard)]

    output_paths = shard_output_paths(output, len(chunks))
    for index, chunk in enumerate(chunks):
        save_shard(output_paths[index], chunk, input_dir, index, len(chunks), args.overwrite)
        print(f"wrote {len(chunk)} sample(s): {output_paths[index]}")

    if args.delete_input:
        shutil.rmtree(input_dir)
        print(f"deleted input directory: {input_dir}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
