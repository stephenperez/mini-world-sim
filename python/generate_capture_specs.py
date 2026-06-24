#!/usr/bin/env python
"""Generate MiniWorldSim capture specs for starter training datasets."""

from __future__ import annotations

import argparse
import json
import math
import random
import shutil
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Wall:
    x1: float
    y1: float
    x2: float
    y2: float
    thickness: float


def load_walls(map_path: Path) -> list[Wall]:
    data = json.loads(map_path.read_text(encoding="utf-8"))
    walls = []
    for wall in data["walls"]:
        walls.append(
            Wall(
                float(wall["x1"]),
                float(wall["y1"]),
                float(wall["x2"]),
                float(wall["y2"]),
                float(wall.get("thickness", 48.0)),
            )
        )
    return walls


def dist_sq_to_segment(px: float, py: float, wall: Wall) -> float:
    sx = wall.x2 - wall.x1
    sy = wall.y2 - wall.y1
    length_sq = sx * sx + sy * sy
    if length_sq <= 1e-6:
        dx = px - wall.x1
        dy = py - wall.y1
        return dx * dx + dy * dy

    t = ((px - wall.x1) * sx + (py - wall.y1) * sy) / length_sq
    t = max(0.0, min(1.0, t))
    cx = wall.x1 + sx * t
    cy = wall.y1 + sy * t
    dx = px - cx
    dy = py - cy
    return dx * dx + dy * dy


def is_clear(x: float, y: float, walls: list[Wall], player_radius: float, margin: float) -> bool:
    for wall in walls:
        radius = player_radius + max(wall.thickness * 0.5, 1.0) + margin
        if dist_sq_to_segment(x, y, wall) <= radius * radius:
            return False
    return True


def world_bounds(walls: list[Wall], inset: float) -> tuple[float, float, float, float]:
    xs = [coord for wall in walls for coord in (wall.x1, wall.x2)]
    ys = [coord for wall in walls for coord in (wall.y1, wall.y2)]
    return min(xs) + inset, max(xs) - inset, min(ys) + inset, max(ys) - inset


def random_clear_pose(
    rng: random.Random,
    walls: list[Wall],
    bounds: tuple[float, float, float, float],
    player_radius: float,
) -> dict[str, float]:
    min_x, max_x, min_y, max_y = bounds
    for _ in range(10_000):
        x = rng.uniform(min_x, max_x)
        y = rng.uniform(min_y, max_y)
        if is_clear(x, y, walls, player_radius, margin=18.0):
            return {"x": round(x, 3), "y": round(y, 3), "yaw_degrees": round(rng.uniform(-180.0, 180.0), 3)}

    raise RuntimeError("could not find clear random pose")


def near_wall_collision_pose(
    rng: random.Random,
    walls: list[Wall],
    player_radius: float,
    collision_forward: float = 1.0,
) -> dict[str, float]:
    interior_walls = [
        wall
        for wall in walls
        if not (
            (abs(wall.x1) >= 2390 and abs(wall.x2) >= 2390)
            or (abs(wall.y1) >= 2390 and abs(wall.y2) >= 2390)
        )
    ] or walls

    for _ in range(1000):
        wall = rng.choice(interior_walls)
        sx = wall.x2 - wall.x1
        sy = wall.y2 - wall.y1
        length = math.hypot(sx, sy)
        if length <= 1e-6:
            continue

        tx = sx / length
        ty = sy / length
        nx = -ty
        ny = tx
        if rng.random() < 0.5:
            nx = -nx
            ny = -ny

        t = rng.uniform(0.12, 0.88)
        base_x = wall.x1 + sx * t
        base_y = wall.y1 + sy * t
        blocked_radius = player_radius + max(wall.thickness * 0.5, 1.0)
        distance_from_wall = blocked_radius + rng.uniform(5.0, 10.0)
        x = base_x + nx * distance_from_wall
        y = base_y + ny * distance_from_wall
        if not is_clear(x, y, walls, player_radius, margin=0.0):
            continue

        if collision_forward < 0.0:
            yaw = math.degrees(math.atan2(ny, nx))
        else:
            yaw = math.degrees(math.atan2(-ny, -nx))
        return {"x": round(x, 3), "y": round(y, 3), "yaw_degrees": round(yaw, 3)}

    raise RuntimeError("could not find near-wall collision pose")


def simulate_try_move(
    x: float,
    y: float,
    dx: float,
    dy: float,
    walls: list[Wall],
    player_radius: float,
) -> tuple[float, float, bool]:
    proposed_x = x + dx
    proposed_y = y + dy
    if is_clear(proposed_x, proposed_y, walls, player_radius, margin=0.0):
        return proposed_x, proposed_y, False

    collided = True
    current_x = x
    current_y = y

    x_only = x + dx
    if is_clear(x_only, y, walls, player_radius, margin=0.0):
        current_x = x_only

    y_only = current_y + dy
    if is_clear(current_x, y_only, walls, player_radius, margin=0.0):
        current_y = y_only

    return current_x, current_y, collided


def angled_collision_pose_action(
    rng: random.Random,
    walls: list[Wall],
    player_radius: float,
    dt: float,
    move_speed: float,
    turn_rate_degrees: float,
) -> tuple[dict[str, float], dict[str, float | str]]:
    interior_walls = [
        wall
        for wall in walls
        if not (
            (abs(wall.x1) >= 2390 and abs(wall.x2) >= 2390)
            or (abs(wall.y1) >= 2390 and abs(wall.y2) >= 2390)
        )
    ] or walls

    move_distance = move_speed * dt
    for _ in range(25_000):
        wall = rng.choice(interior_walls)
        sx = wall.x2 - wall.x1
        sy = wall.y2 - wall.y1
        length = math.hypot(sx, sy)
        if length <= 1e-6:
            continue

        tx = sx / length
        ty = sy / length
        nx = -ty
        ny = tx
        if rng.random() < 0.5:
            nx = -nx
            ny = -ny

        tangent_sign = 1.0 if rng.random() < 0.5 else -1.0
        angle_from_normal = math.radians(rng.uniform(28.0, 62.0))
        forward_x = -nx * math.cos(angle_from_normal) + tx * tangent_sign * math.sin(angle_from_normal)
        forward_y = -ny * math.cos(angle_from_normal) + ty * tangent_sign * math.sin(angle_from_normal)

        t = rng.uniform(0.10, 0.90)
        base_x = wall.x1 + sx * t
        base_y = wall.y1 + sy * t
        blocked_radius = player_radius + max(wall.thickness * 0.5, 1.0)
        distance_from_wall = blocked_radius + rng.uniform(1.0, 4.0)
        x = base_x + nx * distance_from_wall
        y = base_y + ny * distance_from_wall
        if not is_clear(x, y, walls, player_radius, margin=0.0):
            continue

        turn_choice = rng.choices([-1.0, 0.0, 1.0], weights=[1.0, 6.0, 1.0], k=1)[0]
        initial_yaw = math.degrees(math.atan2(forward_y, forward_x)) - turn_choice * turn_rate_degrees * dt
        yaw_after_turn = math.radians(initial_yaw + turn_choice * turn_rate_degrees * dt)
        dx = math.cos(yaw_after_turn) * move_distance
        dy = math.sin(yaw_after_turn) * move_distance
        end_x, end_y, collided = simulate_try_move(x, y, dx, dy, walls, player_radius)
        slide_distance = math.hypot(end_x - x, end_y - y)

        if not collided:
            continue
        if slide_distance <= 1.0 or slide_distance >= move_distance - 0.25:
            continue

        if turn_choice < 0.0:
            label = "angled_collision_forward_turn_left"
        elif turn_choice > 0.0:
            label = "angled_collision_forward_turn_right"
        else:
            label = "angled_collision_forward"

        pose = {
            "x": round(x, 3),
            "y": round(y, 3),
            "yaw_degrees": round(initial_yaw, 3),
        }
        return pose, action(label, forward=1.0, turn=turn_choice, dt=dt)

    raise RuntimeError("could not find angled collision pose")


def action(label: str, forward: float = 0.0, strafe: float = 0.0, turn: float = 0.0, dt: float = 0.05) -> dict[str, float | str]:
    return {
        "label": label,
        "forward": forward,
        "strafe": strafe,
        "turn": turn,
        "duration_seconds": dt,
    }


def choose_action(rng: random.Random, dt: float, profile: str = "balanced") -> dict[str, float | str]:
    p = rng.random()
    if profile == "backward":
        if p < 0.46:
            return action("backward", forward=-1.0, dt=dt)
        if p < 0.64:
            turn = 1.0 if rng.random() < 0.5 else -1.0
            return action("backward_turn", forward=-1.0, turn=turn, dt=dt)
        if p < 0.76:
            turn = 1.0 if rng.random() < 0.5 else -1.0
            return action("turn_right" if turn > 0.0 else "turn_left", turn=turn, dt=dt)
        if p < 0.84:
            return action("forward", forward=1.0, dt=dt)
        if p < 0.93:
            strafe = 1.0 if rng.random() < 0.5 else -1.0
            return action("strafe", strafe=strafe, dt=dt)
        return action("noop", dt=dt)

    if p < 0.34:
        return action("forward", forward=1.0, dt=dt)
    if p < 0.42:
        return action("backward", forward=-1.0, dt=dt)
    if p < 0.67:
        turn = 1.0 if rng.random() < 0.5 else -1.0
        return action("turn_right" if turn > 0.0 else "turn_left", turn=turn, dt=dt)
    if p < 0.82:
        turn = 1.0 if rng.random() < 0.5 else -1.0
        return action("forward_turn", forward=1.0, turn=turn, dt=dt)
    if p < 0.90:
        strafe = 1.0 if rng.random() < 0.5 else -1.0
        return action("strafe", strafe=strafe, dt=dt)
    return action("noop", dt=dt)


def build_spec(
    sample_id: str,
    map_file: str,
    pose: dict[str, float],
    actions: list[dict[str, float | str]],
    minimap_size: int,
    warmup_seconds: float,
) -> dict:
    return {
        "sample_id": sample_id,
        "world": {"map_file": map_file},
        "initial_pose": pose,
        "actions": actions,
        "capture": {
            "include_rgb": True,
            "include_minimap_rgb": True,
            "include_semantic_minimap": True,
            "start_delay_seconds": warmup_seconds,
            "minimap_size": minimap_size,
            "exit_after_capture": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--map", type=Path, default=Path("Content/Levels/default_world.json"))
    parser.add_argument("--map-ref", default="Content/Levels/default_world.json")
    parser.add_argument("--num-samples", type=int, default=5000)
    parser.add_argument("--actions-per-spec", type=int, default=20)
    parser.add_argument("--collision-samples", type=int, default=500)
    parser.add_argument("--backward-collision-samples", type=int, default=100)
    parser.add_argument("--seed", type=int, default=2002)
    parser.add_argument("--dt", type=float, default=0.05)
    parser.add_argument("--player-radius", type=float, default=28.0)
    parser.add_argument("--minimap-size", type=int, default=128)
    parser.add_argument("--warmup-seconds", type=float, default=1.0)
    parser.add_argument("--action-profile", choices=["balanced", "backward", "angled_collision"], default="balanced")
    parser.add_argument("--collision-forward", type=float, choices=[-1.0, 1.0], default=1.0)
    parser.add_argument("--move-speed", type=float, default=260.0)
    parser.add_argument("--turn-rate-degrees", type=float, default=130.0)
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    if args.collision_samples < 0 or args.collision_samples > args.num_samples:
        raise SystemExit("--collision-samples must be between 0 and --num-samples")
    if args.backward_collision_samples < 0 or args.backward_collision_samples > args.collision_samples:
        raise SystemExit("--backward-collision-samples must be between 0 and --collision-samples")

    normal_samples = args.num_samples - args.collision_samples
    if normal_samples % args.actions_per_spec != 0:
        raise SystemExit("non-collision sample count must be divisible by --actions-per-spec")

    output_dir = args.output_dir
    if args.clean and output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    walls = load_walls(args.map)
    bounds = world_bounds(walls, inset=260.0)
    rng = random.Random(args.seed)
    normal_spec_count = normal_samples // args.actions_per_spec

    if args.action_profile == "angled_collision":
        for spec_index in range(args.num_samples):
            pose, angled_action = angled_collision_pose_action(
                rng,
                walls,
                args.player_radius,
                args.dt,
                args.move_speed,
                args.turn_rate_degrees,
            )
            sample_id = f"dev5k_angled_collision_{spec_index:04d}"
            spec = build_spec(
                sample_id,
                args.map_ref,
                pose,
                [angled_action],
                args.minimap_size,
                args.warmup_seconds,
            )
            (output_dir / f"{sample_id}.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")

        print(f"wrote {args.num_samples} angled collision spec file(s), {args.num_samples} sample action(s): {output_dir}")
        return 0

    forward_collision_samples = args.collision_samples - args.backward_collision_samples

    for spec_index in range(forward_collision_samples):
        pose = near_wall_collision_pose(rng, walls, args.player_radius, args.collision_forward)
        sample_id = f"dev5k_collision_{spec_index:04d}"
        collision_label = "collision_backward" if args.collision_forward < 0.0 else "collision_forward"
        spec = build_spec(
            sample_id,
            args.map_ref,
            pose,
            [action(collision_label, forward=args.collision_forward, dt=args.dt)],
            args.minimap_size,
            args.warmup_seconds,
        )
        (output_dir / f"{sample_id}.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")

    for spec_index in range(args.backward_collision_samples):
        pose = near_wall_collision_pose(rng, walls, args.player_radius, -1.0)
        sample_index = forward_collision_samples + spec_index
        sample_id = f"dev5k_collision_{sample_index:04d}"
        spec = build_spec(
            sample_id,
            args.map_ref,
            pose,
            [action("collision_backward", forward=-1.0, dt=args.dt)],
            args.minimap_size,
            args.warmup_seconds,
        )
        (output_dir / f"{sample_id}.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")

    for spec_index in range(normal_spec_count):
        pose = random_clear_pose(rng, walls, bounds, args.player_radius)
        actions = [choose_action(rng, args.dt, args.action_profile) for _ in range(args.actions_per_spec)]
        sample_id = f"dev5k_episode_{spec_index:04d}"
        spec = build_spec(sample_id, args.map_ref, pose, actions, args.minimap_size, args.warmup_seconds)
        (output_dir / f"{sample_id}.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")

    spec_count = args.collision_samples + normal_spec_count
    print(f"wrote {spec_count} spec file(s), {args.num_samples} sample action(s): {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
