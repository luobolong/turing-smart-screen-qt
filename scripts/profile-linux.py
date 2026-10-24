#!/usr/bin/env python3
"""Measure isolated native scenarios; samples process memory, CPU, I/O and maps."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def memory(pid, detailed=False):
    path = Path(f"/proc/{pid}/smaps" if detailed else f"/proc/{pid}/smaps_rollup")
    totals, maps, current = {}, {}, ""
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if "-" in fields[0] and len(fields) >= 5:
            current = " ".join(fields[5:]) or "[anonymous]"
            continue
        if len(fields) >= 2 and fields[0].endswith(":"):
            key = fields[0][:-1]
            if key not in ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Anonymous", "Swap"):
                continue
            value = int(fields[1]) * 1024
            totals[key] = totals.get(key, 0) + value
            if detailed:
                row = maps.setdefault(current, {})
                row[key] = row.get(key, 0) + value
    totals["Uss"] = totals.get("Private_Clean", 0) + totals.get("Private_Dirty", 0)
    if detailed:
        totals["maps"] = sorted(({"path": k, **v} for k, v in maps.items()),
                                key=lambda row: row.get("Rss", 0), reverse=True)
    return totals


def read_process(pid):
    row = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    io = {k: int(v) for k, v in (line.split(":") for line in Path(f"/proc/{pid}/io").read_text().splitlines())}
    return {"cpu_seconds": (int(row[11]) + int(row[12])) / os.sysconf("SC_CLK_TCK"),
            "cpu_user_seconds": int(row[11]) / os.sysconf("SC_CLK_TCK"),
            "cpu_system_seconds": int(row[12]) / os.sysconf("SC_CLK_TCK"),
            "threads": int(row[17]), "io": io, **memory(pid)}


def run_case(args, name, mode="window", scenario="preview", theme="nightfall", **options):
    platform = options.pop("_platform", args.platform)
    scale_factor = options.pop("_scale_factor", args.scale_factor)
    free_size = options.pop("_free_size", args.free_size)
    destination = args.output / name
    destination.mkdir(parents=True, exist_ok=True)
    report = destination / "probe.json"
    command = [str(args.binary), "--mode", mode, "--scenario", scenario, "--seconds", str(args.seconds),
               "--snapshot", str(ROOT / "tests/fixtures/nightfall-sample.json"), "--output", str(report),
               "--data-dir", str(destination / "data")]
    if theme == "nightfall":
        command.extend(["--theme", str(ROOT / "assets/themes/nightfall.json")])
    elif theme:
        command.extend(["--theme", str(Path(theme).resolve())])
    if free_size:
        command.append("--free-size")
    for key, value in options.items():
        command.append("--" + key.replace("_", "-"))
        if value is not True:
            command.append(str(value))
    if mode == "session" and args.port:
        command.extend(["--port", args.port])
    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = platform
    environment["TURING_QT_TRACE"] = str(destination / "startup.jsonl")
    # Keep the tested Qt version's plugins; application style stays Fusion.
    environment["QT_PLUGIN_PATH"] = args.qt_plugins
    if scale_factor is not None:
        environment["QT_SCALE_FACTOR"] = str(scale_factor)
    if not args.inherit_qt_theme:
        environment.pop("QT_QPA_PLATFORMTHEME", None)
        environment.pop("QT_STYLE_OVERRIDE", None)
    (destination / "command.json").write_text(json.dumps(command, ensure_ascii=False, indent=2))
    samples, map_samples = [], []
    start = time.monotonic()
    with (destination / "stdout.txt").open("w") as stdout, (destination / "stderr.txt").open("w") as stderr:
        process = subprocess.Popen(command, env=environment, stdout=stdout, stderr=stderr, start_new_session=True)
        next_map = 5
        while process.poll() is None:
            seconds = time.monotonic() - start
            if seconds > max(120, args.seconds + 30):
                os.killpg(process.pid, signal.SIGTERM)
                raise RuntimeError(f"{name}: timed out")
            try:
                sample = {"seconds": seconds, **read_process(process.pid)}
                samples.append(sample)
                if seconds >= next_map:
                    map_samples.append({"seconds": seconds, **memory(process.pid, detailed=True)})
                    next_map += 5
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                pass
            time.sleep(.5)
        if process.returncode != 0:
            raise RuntimeError(f"{name}: exit {process.returncode}; see {destination / 'stderr.txt'}")
    probe = json.loads(report.read_text())
    if mode == "micro":
        summary = {"name": name, "mode": mode, "seconds": time.monotonic() - start,
                   "peak_rss_mb": max(s["Rss"] for s in samples) / 1e6}
    else:
        steady = json.loads(Path(str(report) + ".steady.json").read_text())
        elapsed = probe["seconds"] - steady["seconds"]
        cpu = probe["ended"]["cpu_seconds"] - steady["process"]["cpu_seconds"]
        final = next(s for s in reversed(samples) if s["Rss"] > 0)
        summary = {"name": name, "mode": mode, "scenario": scenario, "platform": probe["platform"],
                   "seconds": probe["seconds"], "steady_seconds": elapsed,
                   "cpu_one_core_percent": 100 * cpu / elapsed,
                   "cpu_machine_percent": 100 * cpu / elapsed / os.cpu_count(),
                   "peak_rss_mb": max(max(s["Rss"] for s in samples), probe["ended"]["peak_resident_bytes"]) / 1e6,
                   "peak_pss_mb": max(s["Pss"] for s in samples) / 1e6,
                   "peak_uss_mb": max(s["Uss"] for s in samples) / 1e6,
                   "final_rss_mb": final["Rss"] / 1e6, "final_pss_mb": final["Pss"] / 1e6,
                   "final_uss_mb": final["Uss"] / 1e6,
                   "threads": max(s["threads"] for s in samples),
                   "startup_ready_ms": json.loads(Path(str(report) + ".ready.json").read_text())["ready_ms"],
                   "work": probe["work"], "paint_ms": probe["paint_ms"], "actions": probe["actions"]}
        if "window" in probe:
            summary.update(probe["window"])
        if mode == "session":
            for key in ("lightweight", "widget_count", "rendered_frames", "sent_frames", "device_connected",
                        "weather_ready", "image_cache_bytes"):
                if key in probe:
                    summary[key] = probe[key]
        summary.pop("process", None)
        if len(samples) > 1:
            summary["read_syscalls"] = final["io"]["syscr"] - samples[0]["io"]["syscr"]
            summary["write_bytes"] = final["io"]["write_bytes"] - samples[0]["io"]["write_bytes"]
            if "cpu_user_seconds" in final:
                first = next((s for s in samples if s["seconds"] >= 5), samples[0])
                interval = final["seconds"] - first["seconds"]
                if interval > 0:
                    summary["cpu_user_percent"] = 100 * (final["cpu_user_seconds"] - first["cpu_user_seconds"]) / interval
                    summary["cpu_kernel_percent"] = 100 * (final["cpu_system_seconds"] - first["cpu_system_seconds"]) / interval
    (destination / "resources.json").write_text(json.dumps({"summary": summary, "samples": samples,
                                                             "memory_maps": map_samples}, indent=2))
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/profile/screen-profile")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/performance-2026-10-25")
    parser.add_argument("--seconds", type=int, default=25)
    parser.add_argument("--platform", default="wayland" if os.environ.get("WAYLAND_DISPLAY") else
                        "xcb" if os.environ.get("DISPLAY") else "offscreen")
    parser.add_argument("--qt-plugins", help="Override the linked Qt's plugin directory")
    parser.add_argument("--inherit-qt-theme", action="store_true")
    parser.add_argument("--scale-factor", type=float, help="Multiply the platform's display scale (0.5 turns 2x into 1x)")
    parser.add_argument("--case", default="all")
    parser.add_argument("--theme", default="nightfall")
    parser.add_argument("--port", help="Explicit test serial port for lightweight session scenarios")
    parser.add_argument("--free-size", action="store_true")
    parser.add_argument("--stop", action="store_true", help="Stop this probe's runs in the selected output directory")
    args = parser.parse_args()
    if args.seconds < 6:
        parser.error("--seconds must be at least 6 (the first 5 seconds are warm-up)")
    args.binary = args.binary.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.stop:
        for process in Path('/proc').iterdir():
            if not process.name.isdigit() or int(process.name) == os.getpid():
                continue
            try:
                if process.stat().st_uid != os.getuid():
                    continue
                command = (process / 'cmdline').read_bytes().decode().strip('\0').split('\0')
                if '--output' not in command:
                    continue
                output = Path(command[command.index('--output') + 1])
                if not output.is_absolute():
                    output = (process / 'cwd').resolve() / output
                owns_child = command[0] == str(args.binary) and output.is_relative_to(args.output)
                owns_parent = (len(command) > 1 and Path(command[1]).name == Path(__file__).name
                               and output == args.output and '--stop' not in command)
                if owns_child or owns_parent:
                    os.kill(int(process.name), signal.SIGTERM)
                    print('Stopped profiling process', process.name, flush=True)
            except (OSError, ValueError, UnicodeError):
                pass
        return
    if args.qt_plugins is None:
        args.qt_plugins = subprocess.check_output([str(args.binary), '--qt-plugin-path'], text=True).strip()
    cases = [
        ("baseline", {"mode": "baseline", "theme": ""}),
        ("collector", {"mode": "collector", "theme": ""}),
        ("render_nightfall", {"mode": "render"}),
        ("starter", {"theme": ""}),
        ("nightfall", {}),
        ("editor", {"scenario": "editor"}),
        ("sensors", {"scenario": "sensors"}),
        ("settings", {"scenario": "settings"}),
        ("diagnostics", {"scenario": "diagnostics"}),
        ("hidden", {"scenario": "hidden"}),
        ("frozen", {"scenario": "frozen"}),
        ("render_only", {"scenario": "render-only"}),
        ("empty", {"scenario": "empty"}),
        ("light", {"palette": "light"}),
        ("chinese", {"language": "chinese"}),
        ("weather", {"weather": True}),
        ("switch", {"scenario": "switch"}),
        ("scroll", {"scenario": "scroll"}),
        ("edit", {"scenario": "edit"}),
        ("custom", {"theme": args.theme}),
        ("one_x", {"_scale_factor": .5}),
        ("offscreen", {"_platform": "offscreen"}),
        ("unconstrained", {"_free_size": True}),
        ("micro", {"mode": "micro"}),
        ("lightweight", {"mode": "session"}),
        ("lightweight_weather", {"mode": "session", "weather": True}),
        ("lightweight_cycles", {"mode": "session", "scenario": "cycles"}),
    ]
    selected = [(n, o) for n, o in cases if n != 'custom'] if args.case == "all" else [
        (n, o) for n, o in cases if n in args.case.split(",")]
    if not selected:
        parser.error("Unknown case")
    summaries = [run_case(args, name, **options) for name, options in selected]
    (args.output / "summary.json").write_text(json.dumps(summaries, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
