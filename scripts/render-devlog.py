#!/usr/bin/env python3
"""Render a Claude Code session transcript (.jsonl) into a readable dev log.

  python3 scripts/render-devlog.py <session.jsonl> <out.txt>

Keeps the narrative — user turns, assistant reasoning + replies, tool calls
with compact inputs, truncated results — and drops harness bookkeeping
(mode/permission/snapshot/bridge records). Large tool inputs (file writes)
are summarized rather than dumped.
"""
import json
import sys

RESULT_CAP = 800       # chars of any tool result to keep
INPUT_CAP = 1200       # chars of a tool input value to keep
THINK_CAP = 100000     # keep reasoning essentially whole


def trunc(s, n):
    s = str(s)
    return s if len(s) <= n else s[:n] + f"\n      …[+{len(s)-n} chars]"


def fmt_input(name, inp):
    if not isinstance(inp, dict):
        return trunc(inp, INPUT_CAP)
    # Summarize bulky file writes.
    if name in ("Write",) and "content" in inp:
        c = inp.get("content", "")
        return f'file_path={inp.get("file_path")}  ({len(c)} bytes written)'
    parts = []
    for k, v in inp.items():
        vs = v if isinstance(v, str) else json.dumps(v)
        parts.append(f"{k}={trunc(vs, INPUT_CAP)}")
    return "  ".join(parts)


def text_of(content):
    """Flatten a tool_result content field to text."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        out = []
        for b in content:
            if isinstance(b, dict):
                out.append(b.get("text", b.get("content", "")))
            else:
                out.append(str(b))
        return "\n".join(str(x) for x in out)
    return str(content)


def main():
    src, out = sys.argv[1], sys.argv[2]
    lines = []
    turn = 0
    for raw in open(src):
        raw = raw.strip()
        if not raw:
            continue
        try:
            d = json.loads(raw)
        except json.JSONDecodeError:
            continue
        t = d.get("type")
        if t not in ("user", "assistant"):
            continue
        msg = d.get("message", {}) or {}
        role = msg.get("role")
        content = msg.get("content")

        if role == "user" and isinstance(content, str):
            s = content.strip()
            if not s:
                continue
            turn += 1
            lines.append("\n" + "=" * 78)
            lines.append(f"USER  [turn {turn}]")
            lines.append("=" * 78)
            lines.append(s)
            continue

        if not isinstance(content, list):
            continue

        for b in content:
            if not isinstance(b, dict):
                continue
            bt = b.get("type")
            if bt == "thinking":
                th = b.get("thinking", "").strip()
                if th:
                    lines.append("\n  ┌─ reasoning ─")
                    for ln in trunc(th, THINK_CAP).splitlines():
                        lines.append("  │ " + ln)
            elif bt == "text":
                tx = b.get("text", "").strip()
                if tx:
                    lines.append("\nCLAUDE:")
                    lines.append(tx)
            elif bt == "tool_use":
                lines.append(f"\n  → {b.get('name')}({fmt_input(b.get('name'), b.get('input'))})")
            elif bt == "tool_result":
                body = text_of(b.get("content"))
                if body.strip():
                    lines.append("    ⮑ " + trunc(body, RESULT_CAP).replace("\n", "\n      "))

    with open(out, "w") as f:
        f.write("Chimera.APE — Alpha Development Log\n")
        f.write("Rendered from the Claude Code session transcript.\n")
        f.write(f"Turns: {turn}\n")
        f.write("\n".join(lines))
        f.write("\n")
    print(f"wrote {out} ({turn} user turns)")


if __name__ == "__main__":
    main()
