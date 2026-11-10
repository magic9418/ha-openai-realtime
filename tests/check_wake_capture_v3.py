#!/usr/bin/env python3
"""Validate the frames printed by tests/test_wake_capture_v3.cpp against the v3 protocol with Python."""
import base64, json, sys, zlib
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
start = lines[0]
assert start["capture_version"] == 3 and start["capture_only"] is True and start["features_follow"] is True
assert start["stage2"] == {"model": "penny_stage2_v1", "mode": "shadow", "score": 77, "threshold": 15, "verdict": "pass", "infer_ms": 12}
assert "stage2" not in lines[1] and lines[1]["capture_version"] == 3 and "capture_only" not in lines[1]
fs = [l for l in lines[2:]]
assert fs[0]["type"] == "wake_features_start" and fs[0]["frames"] == 248 and fs[0]["fire_frame"] == 197 and fs[0]["channels"] == 40
assert fs[0]["fire_frame"] == fs[0]["frames"] - 1 - fs[0]["post_frames"]
assert len(fs[0]["probs"]) == len(fs[0]["vad_probs"]) == 248 // 3
raw = b""
for i, c in enumerate(fs[1:-1]):
    assert c["type"] == "wake_features_chunk" and c["seq"] == i
    d = base64.b64decode(c["b64"]); assert len(d) <= 1024; raw += d
end = fs[-1]
assert end["type"] == "wake_features_end" and end["bytes"] == len(raw) == 248 * 40 and end["crc32"] == zlib.crc32(raw)
print("capture v3 frames OK:", len(fs), "feature frames,", len(raw), "bytes, crc", end["crc32"])
