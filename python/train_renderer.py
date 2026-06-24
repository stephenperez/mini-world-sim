#!/usr/bin/env python
"""Train a starter MiniWorldSim neural renderer from packed .pt shards.

Initial task:
    minimap_semantic_t1 -> rgb_t1

This deliberately starts with the map-only renderer so the data/training/server
pipeline can be proven before adding temporal conditioning.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset, Subset


@dataclass
class TrainConfig:
    data: list[str]
    out_dir: str
    resume: str
    resume_weights_only: bool
    reset_best: bool
    epochs: int
    batch_size: int
    lr: float
    weight_decay: float
    base_channels: int
    val_fraction: float
    seed: int
    num_workers: int
    amp: str
    compile: bool
    max_train_samples: int
    max_val_samples: int
    preview_every: int
    save_every: int


def find_shards(paths: Iterable[str]) -> list[Path]:
    shards: list[Path] = []
    for raw_path in paths:
        path = Path(raw_path)
        if any(char in raw_path for char in "*?[]"):
            shards.extend(sorted(path.parent.glob(path.name)))
        elif path.is_dir():
            shards.extend(sorted(path.glob("*.pt")))
        else:
            shards.append(path)

    shards = [path.resolve() for path in shards]
    missing = [path for path in shards if not path.exists()]
    if missing:
        raise FileNotFoundError(f"Missing shard(s): {missing}")
    if not shards:
        raise FileNotFoundError("No .pt shards found.")
    return shards


class PackedCaptureDataset(Dataset):
    def __init__(self, shard_paths: list[Path]):
        self.samples: list[dict] = []
        self.shard_summaries: list[dict] = []
        for shard_path in shard_paths:
            payload = torch.load(shard_path, map_location="cpu")
            samples = payload.get("samples", [])
            self.samples.extend(samples)
            self.shard_summaries.append(
                {
                    "path": str(shard_path),
                    "num_samples": len(samples),
                    "format": payload.get("format"),
                    "format_version": payload.get("format_version"),
                }
            )

        if not self.samples:
            raise ValueError("Loaded shards contain no samples.")

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> dict[str, torch.Tensor | str]:
        sample = self.samples[index]
        # Semantic minimap is uint8 CHW. Convert to normalized float.
        x = sample["minimap_semantic_t1"].float() / 255.0
        y = sample["rgb_t1"].float() / 255.0
        action = sample["action"].float()
        motion = sample["actual_motion"].float()
        return {
            "x": x,
            "y": y,
            "action": action,
            "motion": motion,
            "sample_id": sample.get("sample_id", str(index)),
        }


class ConvBlock(nn.Module):
    def __init__(self, in_channels: int, out_channels: int):
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv2d(in_channels, out_channels, kernel_size=3, padding=1),
            nn.GroupNorm(num_groups=min(8, out_channels), num_channels=out_channels),
            nn.SiLU(inplace=True),
            nn.Conv2d(out_channels, out_channels, kernel_size=3, padding=1),
            nn.GroupNorm(num_groups=min(8, out_channels), num_channels=out_channels),
            nn.SiLU(inplace=True),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)


class DownBlock(nn.Module):
    def __init__(self, in_channels: int, out_channels: int):
        super().__init__()
        self.block = ConvBlock(in_channels, out_channels)
        self.down = nn.Conv2d(out_channels, out_channels, kernel_size=4, stride=2, padding=1)

    def forward(self, x: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        skip = self.block(x)
        return skip, self.down(skip)


class UpBlock(nn.Module):
    def __init__(self, in_channels: int, skip_channels: int, out_channels: int):
        super().__init__()
        self.up = nn.ConvTranspose2d(in_channels, out_channels, kernel_size=4, stride=2, padding=1)
        self.block = ConvBlock(out_channels + skip_channels, out_channels)

    def forward(self, x: torch.Tensor, skip: torch.Tensor) -> torch.Tensor:
        x = self.up(x)
        if x.shape[-2:] != skip.shape[-2:]:
            x = F.interpolate(x, size=skip.shape[-2:], mode="bilinear", align_corners=False)
        return self.block(torch.cat([x, skip], dim=1))


class MiniUNet(nn.Module):
    def __init__(self, in_channels: int = 6, out_channels: int = 3, base_channels: int = 48):
        super().__init__()
        c = base_channels
        self.down1 = DownBlock(in_channels, c)
        self.down2 = DownBlock(c, c * 2)
        self.down3 = DownBlock(c * 2, c * 4)
        self.down4 = DownBlock(c * 4, c * 8)
        self.mid = ConvBlock(c * 8, c * 8)
        self.up4 = UpBlock(c * 8, c * 8, c * 4)
        self.up3 = UpBlock(c * 4, c * 4, c * 2)
        self.up2 = UpBlock(c * 2, c * 2, c)
        self.up1 = UpBlock(c, c, c)
        self.out = nn.Sequential(
            nn.Conv2d(c, c, kernel_size=3, padding=1),
            nn.SiLU(inplace=True),
            nn.Conv2d(c, out_channels, kernel_size=1),
            nn.Sigmoid(),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        s1, x = self.down1(x)
        s2, x = self.down2(x)
        s3, x = self.down3(x)
        s4, x = self.down4(x)
        x = self.mid(x)
        x = self.up4(x, s4)
        x = self.up3(x, s3)
        x = self.up2(x, s2)
        x = self.up1(x, s1)
        return self.out(x)


def split_indices(length: int, val_fraction: float, seed: int) -> tuple[list[int], list[int]]:
    indices = list(range(length))
    rng = random.Random(seed)
    rng.shuffle(indices)
    val_count = max(1, int(round(length * val_fraction))) if length > 1 else 0
    val_indices = indices[:val_count]
    train_indices = indices[val_count:]
    return train_indices, val_indices


def limit_indices(indices: list[int], limit: int) -> list[int]:
    if limit and limit > 0:
        return indices[:limit]
    return indices


def make_grid(images: torch.Tensor, rows: int) -> torch.Tensor:
    images = images.detach().cpu().clamp(0, 1)
    n, c, h, w = images.shape
    cols = math.ceil(n / rows)
    grid = torch.zeros(c, rows * h, cols * w)
    for idx in range(n):
        row = idx // cols
        col = idx % cols
        grid[:, row * h : (row + 1) * h, col * w : (col + 1) * w] = images[idx]
    return grid


def save_png(path: Path, image_chw: torch.Tensor) -> None:
    from PIL import Image

    image_hwc = (image_chw.permute(1, 2, 0).clamp(0, 1) * 255.0).byte().numpy()
    Image.fromarray(image_hwc, mode="RGB").save(path)


def preview(model: nn.Module, batch: dict, device: torch.device, out_path: Path, amp_dtype: torch.dtype | None) -> None:
    model.eval()
    x = batch["x"].to(device, non_blocking=True)
    target = batch["y"].to(device, non_blocking=True)
    with torch.no_grad():
        if amp_dtype is None:
            pred = model(x)
        else:
            with torch.autocast(device_type=device.type, dtype=amp_dtype):
                pred = model(x)

    count = min(6, pred.shape[0])
    comparison = torch.cat([target[:count], pred[:count]], dim=0)
    save_png(out_path, make_grid(comparison, rows=2))
    model.train()


def choose_amp_dtype(device: torch.device, amp: str) -> torch.dtype | None:
    if device.type != "cuda" or amp == "none":
        return None
    if amp == "bf16":
        return torch.bfloat16
    if amp == "fp16":
        return torch.float16
    if torch.cuda.is_bf16_supported():
        return torch.bfloat16
    return torch.float16


def move_optimizer_state_to_device(optimizer: torch.optim.Optimizer, device: torch.device) -> None:
    for state in optimizer.state.values():
        for key, value in list(state.items()):
            if torch.is_tensor(value):
                state[key] = value.to(device)


def train(args: argparse.Namespace) -> None:
    shard_paths = find_shards(args.data)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "previews").mkdir(exist_ok=True)

    resume_path = Path(args.resume).resolve() if args.resume else None
    resume_checkpoint = None
    if resume_path is not None:
        if not resume_path.exists():
            raise FileNotFoundError(f"Resume checkpoint does not exist: {resume_path}")
        resume_checkpoint = torch.load(resume_path, map_location="cpu", weights_only=False)
        resume_config = resume_checkpoint.get("config", {})
        if args.base_channels is None:
            args.base_channels = int(resume_config.get("base_channels", 48))
    if args.base_channels is None:
        args.base_channels = 48

    torch.manual_seed(args.seed)
    random.seed(args.seed)

    dataset = PackedCaptureDataset(shard_paths)
    train_indices, val_indices = split_indices(len(dataset), args.val_fraction, args.seed)
    train_indices = limit_indices(train_indices, args.max_train_samples)
    val_indices = limit_indices(val_indices, args.max_val_samples)

    train_loader = DataLoader(
        Subset(dataset, train_indices),
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        pin_memory=torch.cuda.is_available(),
    )
    val_loader = DataLoader(
        Subset(dataset, val_indices),
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        pin_memory=torch.cuda.is_available(),
    )

    device = torch.device("cuda" if torch.cuda.is_available() and not args.cpu else "cpu")
    amp_dtype = choose_amp_dtype(device, args.amp)
    model = MiniUNet(in_channels=6, out_channels=3, base_channels=args.base_channels).to(device)
    if args.channels_last and device.type == "cuda":
        model = model.to(memory_format=torch.channels_last)

    resume_epoch = 0
    resume_global_step = 0
    if resume_checkpoint is not None:
        model.load_state_dict(resume_checkpoint["model"])
        resume_epoch = int(resume_checkpoint.get("epoch", 0))
        resume_global_step = int(resume_checkpoint.get("global_step", 0))
        print(f"resumed model from {resume_path} at epoch={resume_epoch} global_step={resume_global_step}")

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    optimizer_loaded = False
    if resume_checkpoint is not None and not args.resume_weights_only and "optimizer" in resume_checkpoint:
        optimizer.load_state_dict(resume_checkpoint["optimizer"])
        move_optimizer_state_to_device(optimizer, device)
        optimizer_loaded = True

    scaler = torch.amp.GradScaler("cuda", enabled=(amp_dtype == torch.float16))
    if args.compile:
        model = torch.compile(model)

    config = TrainConfig(
        data=[str(path) for path in shard_paths],
        out_dir=str(out_dir),
        resume=str(resume_path) if resume_path is not None else "",
        resume_weights_only=args.resume_weights_only,
        reset_best=args.reset_best,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        base_channels=args.base_channels,
        val_fraction=args.val_fraction,
        seed=args.seed,
        num_workers=args.num_workers,
        amp=args.amp,
        compile=args.compile,
        max_train_samples=args.max_train_samples,
        max_val_samples=args.max_val_samples,
        preview_every=args.preview_every,
        save_every=args.save_every,
    )
    metadata = {
        "config": asdict(config),
        "shards": dataset.shard_summaries,
        "num_samples": len(dataset),
        "num_train": len(train_indices),
        "num_val": len(val_indices),
        "device": str(device),
        "amp_dtype": str(amp_dtype),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "gpu": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
        "resume": {
            "path": str(resume_path) if resume_path is not None else "",
            "checkpoint_epoch": resume_epoch,
            "checkpoint_global_step": resume_global_step,
            "optimizer_loaded": optimizer_loaded,
        },
    }
    (out_dir / "run_config.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    print(json.dumps(metadata, indent=2))
    best_val = float("inf")
    if resume_checkpoint is not None and not args.reset_best:
        best_val = float(resume_checkpoint.get("val_loss", best_val))
    global_step = 0 if args.resume_weights_only else resume_global_step
    start_epoch = resume_epoch + 1
    start_time = time.perf_counter()

    fixed_preview_batch = next(iter(val_loader if len(val_indices) else train_loader))

    for local_epoch in range(1, args.epochs + 1):
        epoch = start_epoch + local_epoch - 1
        model.train()
        train_loss_sum = 0.0
        train_count = 0
        epoch_start = time.perf_counter()

        for batch in train_loader:
            global_step += 1
            x = batch["x"].to(device, non_blocking=True)
            y = batch["y"].to(device, non_blocking=True)
            if args.channels_last and device.type == "cuda":
                x = x.contiguous(memory_format=torch.channels_last)
                y = y.contiguous(memory_format=torch.channels_last)

            optimizer.zero_grad(set_to_none=True)
            if amp_dtype is None:
                pred = model(x)
                loss = F.l1_loss(pred, y) + 0.25 * F.mse_loss(pred, y)
                loss.backward()
                optimizer.step()
            else:
                with torch.autocast(device_type=device.type, dtype=amp_dtype):
                    pred = model(x)
                    loss = F.l1_loss(pred, y) + 0.25 * F.mse_loss(pred, y)
                scaler.scale(loss).backward()
                scaler.step(optimizer)
                scaler.update()

            batch_size = x.shape[0]
            train_loss_sum += float(loss.detach().cpu()) * batch_size
            train_count += batch_size

            if args.preview_every > 0 and global_step % args.preview_every == 0:
                preview(
                    model,
                    fixed_preview_batch,
                    device,
                    out_dir / "previews" / f"step_{global_step:06d}.png",
                    amp_dtype,
                )

        train_loss = train_loss_sum / max(train_count, 1)
        val_loss = evaluate(model, val_loader, device, amp_dtype, channels_last=args.channels_last)
        elapsed = time.perf_counter() - epoch_start
        print(
            f"epoch={epoch:03d} train_loss={train_loss:.6f} "
            f"val_loss={val_loss:.6f} seconds={elapsed:.1f}"
        )

        checkpoint = {
            "model": model._orig_mod.state_dict() if hasattr(model, "_orig_mod") else model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "epoch": epoch,
            "global_step": global_step,
            "train_loss": train_loss,
            "val_loss": val_loss,
            "config": asdict(config),
        }
        torch.save(checkpoint, out_dir / "last.pt")
        if val_loss < best_val:
            best_val = val_loss
            torch.save(checkpoint, out_dir / "best.pt")
        if args.save_every > 0 and local_epoch % args.save_every == 0:
            torch.save(checkpoint, out_dir / f"epoch_{epoch:03d}.pt")

        preview(
            model,
            fixed_preview_batch,
            device,
            out_dir / "previews" / f"epoch_{epoch:03d}.png",
            amp_dtype,
        )

    total_elapsed = time.perf_counter() - start_time
    print(f"done total_seconds={total_elapsed:.1f} best_val={best_val:.6f}")


def evaluate(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    amp_dtype: torch.dtype | None,
    channels_last: bool,
) -> float:
    if len(loader.dataset) == 0:
        return float("nan")

    model.eval()
    loss_sum = 0.0
    count = 0
    with torch.no_grad():
        for batch in loader:
            x = batch["x"].to(device, non_blocking=True)
            y = batch["y"].to(device, non_blocking=True)
            if channels_last and device.type == "cuda":
                x = x.contiguous(memory_format=torch.channels_last)
                y = y.contiguous(memory_format=torch.channels_last)
            if amp_dtype is None:
                pred = model(x)
                loss = F.l1_loss(pred, y) + 0.25 * F.mse_loss(pred, y)
            else:
                with torch.autocast(device_type=device.type, dtype=amp_dtype):
                    pred = model(x)
                    loss = F.l1_loss(pred, y) + 0.25 * F.mse_loss(pred, y)
            batch_size = x.shape[0]
            loss_sum += float(loss.detach().cpu()) * batch_size
            count += batch_size
    model.train()
    return loss_sum / max(count, 1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", nargs="+", required=True, help="One or more .pt shards, shard dirs, or glob patterns.")
    parser.add_argument("--out-dir", default="Saved/Training/map_only")
    parser.add_argument("--resume", default="", help="Checkpoint to continue from, for example Saved/Training/run/best.pt.")
    parser.add_argument("--resume-weights-only", action="store_true", help="Load model weights but start a fresh optimizer/global step.")
    parser.add_argument("--reset-best", action="store_true", help="When resuming, let the first resumed epoch establish best.pt.")
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--lr", type=float, default=3e-4)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--base-channels", type=int, default=None)
    parser.add_argument("--val-fraction", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--num-workers", type=int, default=0)
    parser.add_argument("--amp", choices=["auto", "bf16", "fp16", "none"], default="auto")
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--channels-last", action="store_true")
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--max-train-samples", type=int, default=0)
    parser.add_argument("--max-val-samples", type=int, default=0)
    parser.add_argument("--preview-every", type=int, default=100)
    parser.add_argument("--save-every", type=int, default=0)
    return parser.parse_args()


if __name__ == "__main__":
    train(parse_args())
