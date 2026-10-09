#!/usr/bin/env python3
"""Bounded Vulkan capture analysis. Observations never imply WoW semantics.

Usage: discovery-report.py capture.json [--compare control.json] [--output report.md]
Compare an unchanged camera with water visible/hidden; visually isolate candidates
before calling any of them water. No categories are inferred from shader hashes.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


def load(path):
    path = Path(path)
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError("capture exceeds 128 MiB analysis limit")
    raw = path.read_bytes()
    capture = json.loads(raw)
    if capture.get("schema") != 1 or not isinstance(capture.get("frames"), list):
        raise ValueError("unsupported capture schema")
    capture["_path"] = str(path)
    capture["_sha256"] = hashlib.sha256(raw).hexdigest()
    return capture


def groups(capture):
    pipelines = {p["id"]: p for p in capture["pipelines"]}
    result = {}
    roles = {0: "unknown", 1: "swapchain-color", 2: "offscreen-color", 3: "depth-only", 4: "mixed"}
    for frame in capture["frames"]:
        for draw in frame["draws"]:
            pipeline = pipelines.get(draw["pipeline"])
            if not pipeline:
                continue
            key = pipeline["hash"]
            row = result.setdefault(key, dict(pipeline=pipeline, ids=set(), count=0, skipped=0,
                                             roles=set(), writes=set(), blends=set(), kinds=Counter(), textures=set()))
            row["ids"].add(pipeline["id"])
            row["count"] += 1
            row["skipped"] += bool(draw["skipped"])
            row["roles"].add(roles.get(draw["target"]["role"], "unknown"))
            row["writes"].add(draw["depthWrite"])
            row["blends"].add(draw.get("blend"))
            row["kinds"][draw["kind"]] += 1
            for resource in draw["resources"]:
                view = resource.get("view")
                if view:
                    row["textures"].add((view.get("format"), view.get("width"), view.get("height"),
                                         view.get("layers"), view.get("mips")))
    return result


def state_values(values):
    return "/".join("unknown" if v is None else str(v) for v in sorted(values, key=lambda v: -1 if v is None else v))


def transitions(capture):
    """Adjacent pipeline-run transitions, preserving successful submit order."""
    pipelines = {p["id"]: p["hash"] for p in capture["pipelines"]}
    pairs = Counter()
    for frame in capture["frames"]:
        previous = None
        for draw in frame["draws"]:
            current = pipelines.get(draw["pipeline"])
            if current != previous:
                if previous and current:
                    pairs[(previous, current)] += 1
                previous = current
    return pairs


def overlaps(a, b):
    if a.get("imageGeneration") != b.get("imageGeneration") or a.get("image") != b.get("image"):
        return False
    if not (a["aspect"] & b["aspect"]):
        return False
    for base, count, limit in (("baseMip", "mipCount", "mips"), ("baseLayer", "layerCount", "layers")):
        aend = a.get(limit, 0) if a[count] == 0xFFFFFFFF else a[base] + a[count]
        bend = b.get(limit, 0) if b[count] == 0xFFFFFFFF else b[base] + b[count]
        if max(a[base], b[base]) >= min(aend, bend):
            return False
    return True


def depth_links(capture):
    """Same queue/interval, earlier depth-write draw -> later bound depth view.

    This proves recorded binding/order, not fragment execution or shadow meaning.
    No cross-frame, cross-queue, aliased-memory or copy propagation is guessed.
    """
    pipelines = {p["id"]: p for p in capture["pipelines"]}
    links = {}
    for frame in capture["frames"]:
        producers = defaultdict(dict)
        for draw in frame["draws"]:
            p = pipelines.get(draw["pipeline"])
            if not p or draw["skipped"]:
                continue
            for resource in draw["resources"]:
                view = resource.get("view")
                if not (view and view.get("imageGeneration") and view["aspect"] & 2 and resource["known"]):
                    continue
                if resource["type"] not in (1, 2, 10):  # combined, sampled, input attachment
                    continue
                for (producer, _), target in producers[view["imageGeneration"]].items():
                    if not overlaps(target, view):
                        continue
                    key = (view["imageGeneration"], producer, p["hash"], resource["set"], resource["binding"])
                    link = links.setdefault(key, dict(view=view, producer=producer, consumer=p["hash"],
                                                      binding=(resource["set"], resource["binding"]), count=0,
                                                      dref=False, sampled=False, compare=False, uab=False,
                                                      filters=set(), uncertain=False))
                    link["count"] += 1
                    link["dref"] |= bool(resource["staticDref"])
                    link["sampled"] |= bool(resource["staticSample"])
                    link["uab"] |= bool(resource["updateAfterBind"])
                    link["uncertain"] |= bool(draw["descriptorsIncomplete"] or draw["unsupportedState"] or frame["truncated"] or frame["missingCommands"])
                    if sampler := resource.get("sampler"):
                        link["compare"] |= bool(sampler["compareEnable"])
                        link["filters"].add((sampler["minFilter"], sampler["magFilter"]))
            target = draw["target"]
            depth = target.get("depth")
            if (target["known"] and depth and depth.get("imageGeneration") and draw["depthTest"] == 1
                    and draw["depthWrite"] == 1 and draw["rasterDiscard"] == 0 and not draw["unsupportedState"]):
                subrange = (depth["baseMip"], depth["mipCount"], depth["baseLayer"], depth["layerCount"])
                producers[depth["imageGeneration"]][(p["hash"], subrange)] = depth
    return list(links.values())


def report(capture, control=None):
    data = groups(capture)
    frames = len(capture["frames"])
    incomplete = sum(bool(f["truncated"] or f["missingCommands"]) for f in capture["frames"])
    lines = ["# Milestone A capture report", "", "Status: PROTOTYPE — semantic categories remain unknown.", "",
             f"Capture: `{capture['_path']}`", f"SHA-256: `{capture['_sha256']}`", "",
             f"Present intervals: {frames}; incomplete intervals: {incomplete}. Counts are recorded draw calls in successful submissions, not rasterized geometry.", "",
             "## Pipeline groups", "", "| IDs | Fingerprint | VS / FS | draws/interval | blend | depth write | target |", "|---|---|---|---:|---|---|---|"]
    for key, row in sorted(data.items(), key=lambda item: (-item[1]["count"], item[0])):
        p = row["pipeline"]
        lines.append(f"| {','.join(map(str, sorted(row['ids'])))} | `{key}` | `{p['vs']}` / `{p['fs']}` | {row['count']/max(frames,1):.2f} | {state_values(row['blends'])} | {state_values(row['writes'])} | {', '.join(sorted(row['roles']))} |")
    lines += ["", "Fingerprint groups cover captured fields; incomplete fingerprints and hash collisions cannot establish identical pipelines. Indirect draw count/instance data remains GPU-unknown. A depth/texture binding is not proof that a fragment executed a sample.", "", "## Repeated draw ordering", ""]
    lines += [f"- `{a}` → `{b}`: {n} adjacent pipeline-run transitions." for (a,b),n in transitions(capture).most_common(20)] or ["No pipeline-run transitions captured."]
    lines += ["", "These are recorded sequence patterns, not render-stage semantics.", "", "## Shadow producer / consumer candidates", ""]
    links = depth_links(capture)
    if not links:
        lines.append("No within-interval depth-write → bound depth-read relationship was found. This does not prove that shadows are absent.")
    for link in sorted(links, key=lambda x: (-x["count"], x["producer"], x["consumer"])):
        v = link["view"]
        lines.append(f"- Image `{v['image']}` generation {v['imageGeneration']}, {v.get('width')}×{v.get('height')}, format {v.get('format')}, layers {v.get('layers')}: producer `{link['producer']}` → consumer `{link['consumer']}` set/binding {link['binding']}; observations {link['count']}; static sample={link['sampled']}, Dref={link['dref']}, combined compare sampler={link['compare']}, filter enums={sorted(link['filters'])}, update-after-bind={link['uab']}, incomplete={link['uncertain']}.")
    lines += ["", f"Distinct linked depth resources in this capture: {len({x['view']['imageGeneration'] for x in links})}. These are shadow candidates, not confirmed shadow maps.",
              "Directional light: unknown. Cascades: unknown. Per-pixel PCF sample count: unknown. Penumbra/PCSS: unknown. Shader bias computation: unknown. Raster depth bias, compare state and sampler filter enums are in the JSON; sampler mip LOD bias is not shadow bias.", "", "## Static shader analysis", "",
              "| Hash | Sample opcodes | Dref opcodes | Gather opcodes | Fetch/read opcodes | Unresolved sample roots |", "|---|---:|---:|---:|---:|---:|"]
    for shader in capture["shaders"]:
        lines.append(f"| `{shader['hash']}` | {shader['staticSamples']} | {shader['staticDrefs']} | {shader['staticGathers']} | {shader['staticFetches']} | {shader['unresolvedSamples']} |")
    lines += ["", "Static opcode totals cover a module, including branches/functions; they are not runtime sample counts. SPIR-V dumps allow focused shader inspection. Separate sampler/image association and texture content semantics are not inferred.", "", "## Water candidates", ""]
    if control:
        other = groups(control)
        count = max(1, len(control["frames"]))
        lines += [f"Comparison: `{control['_path']}` (SHA-256 `{control['_sha256']}`).", "",
                  "Candidates below changed between captures. Camera, UI, animation, particles and visibility can produce the same signal; none is automatically classified as water.", "",
                  "| Fingerprint | Present / control draws per interval | Difference | Visible-capture IDs | Blend | Changed signals |", "|---|---:|---:|---|---|---|"]
        differences = []
        for key in data.keys() | other.keys():
            current = data.get(key, {}).get("count", 0)/max(1, frames)
            previous = other.get(key, {}).get("count", 0)/count
            signals = []
            if current != previous: signals.append("draw count")
            if key in data and key in other:
                for field in ("roles", "writes", "blends", "textures"):
                    if data[key][field] != other[key][field]: signals.append(field)
            if signals:
                differences.append((abs(current-previous), key, current, previous, signals))
        for _, key, current, previous, signals in sorted(differences, reverse=True)[:40]:
            row = data.get(key)
            lines.append(f"| `{key}` | {current:.2f} / {previous:.2f} | {current-previous:+.2f} | {','.join(map(str, sorted(row['ids']))) if row else '-'} | {state_values(row['blends']) if row else '-'} | {', '.join(signals)} |")
        lines.append("Texture signals compare format/extent/layer/mip patterns, not pixels, animation, normal-map semantics or handles across sessions.")
    else:
        lines.append("A water-visible capture and comparable control capture are required. Re-run with `--compare control.json`, then visually isolate the changed pipeline IDs in the game.")
    lines += ["", "## Acceptance status", "", "Confirmed pipelines:", "- None from instrumentation alone.", "",
              "Likely pipelines:"]
    lines += [f"- `{p}`: depth producer/consumer candidate, semantic meaning unconfirmed." for p in sorted({v for link in links for v in (link["producer"], link["consumer"])})] or ["- None established."]
    lines += ["", "Unknown pipelines:"] + [f"- `{key}` (terrain/character/static/foliage/particles/sky/water/UI semantics unknown)." for key in sorted(data)]
    lines += ["", "Next step: keep camera and settings controlled, isolate the candidate in the game, retain screenshot/video plus capture hashes and pipeline ID. CONFIRMED requires that visual evidence; this tool never emits WATER-PIPELINE-CONFIRMED.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture")
    parser.add_argument("--compare")
    parser.add_argument("--output")
    args = parser.parse_args()
    text = report(load(args.capture), load(args.compare) if args.compare else None)
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
