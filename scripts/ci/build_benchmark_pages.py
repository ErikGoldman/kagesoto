#!/usr/bin/env python3
"""Build the static benchmark pages and historical index."""

from __future__ import annotations

import argparse
import json
import shutil
import urllib.error
import urllib.request
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site-dir", required=True, type=Path)
    parser.add_argument("--results-dir", required=True, type=Path)
    parser.add_argument("--history-url", default="")
    return parser.parse_args()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def fetch_existing_history(url: str) -> list[dict]:
    if not url:
        return []
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            data = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
        return []
    if isinstance(data, dict) and isinstance(data.get("runs"), list):
        return data["runs"]
    return []


def compact_run(result: dict) -> dict:
    commit = result.get("commit", "")
    return {
        "commit": commit,
        "short_commit": commit[:12],
        "ref": result.get("ref", ""),
        "run_id": result.get("run_id", ""),
        "run_attempt": result.get("run_attempt", ""),
        "started_at": result.get("started_at", ""),
        "finished_at": result.get("finished_at", ""),
        "subset": result.get("subset", ""),
        "benchmark_filter": result.get("benchmark_filter", ""),
        "path": f"commits/{commit}/result.json",
        "cpu": result.get("cpu", []),
    }


def merge_history(existing: list[dict], current: dict) -> list[dict]:
    merged: dict[str, dict] = {}
    for item in existing:
        key = str(item.get("commit", ""))
        if key:
            merged[key] = item
    merged[str(current["commit"])] = current
    runs = sorted(merged.values(), key=lambda item: str(item.get("started_at", "")), reverse=True)
    return runs[:250]


def write_index_html(path: Path, history: dict) -> None:
    history_json = json.dumps(history, separators=(",", ":"))
    path.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kagesoko ECS Benchmarks</title>
<style>
:root {{
  color-scheme: light dark;
  --bg: #f6f7f5;
  --fg: #17191b;
  --muted: #626970;
  --panel: #ffffff;
  --line: #d7dce0;
  --accent: #006b5f;
  --bad: #b33c22;
  --good: #26733d;
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    --bg: #101214;
    --fg: #f1f3f4;
    --muted: #a5adb3;
    --panel: #181b1f;
    --line: #31363b;
    --accent: #2bb5a5;
    --bad: #e06a52;
    --good: #65b87b;
  }}
}}
* {{ box-sizing: border-box; }}
body {{
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 14px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}}
main {{ max-width: 1180px; margin: 0 auto; padding: 28px 20px 56px; }}
h1 {{ font-size: 28px; margin: 0 0 4px; letter-spacing: 0; }}
h2 {{ font-size: 18px; margin: 28px 0 10px; }}
h3 {{ font-size: 14px; margin: 18px 0 6px; }}
p {{ margin: 0 0 18px; color: var(--muted); }}
.toolbar {{ display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 18px 0; }}
select, input {{
  min-height: 34px;
  border: 1px solid var(--line);
  border-radius: 6px;
  background: var(--panel);
  color: var(--fg);
  padding: 6px 10px;
}}
table {{
  width: 100%;
  border-collapse: collapse;
  background: var(--panel);
  border: 1px solid var(--line);
}}
th, td {{
  padding: 8px 10px;
  border-bottom: 1px solid var(--line);
  text-align: left;
  vertical-align: top;
}}
th {{ font-size: 12px; color: var(--muted); font-weight: 650; }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
a {{ color: var(--accent); }}
.empty {{ padding: 18px; border: 1px solid var(--line); background: var(--panel); color: var(--muted); }}
.chart {{
  border: 1px solid var(--line);
  background: var(--panel);
  margin-bottom: 12px;
}}
.chart svg {{ display: block; width: 100%; height: auto; min-height: 220px; }}
.axis, .gridline {{ stroke: var(--line); stroke-width: 1; }}
.axis-label {{ fill: var(--muted); font-size: 11px; }}
.legend {{ display: flex; flex-wrap: wrap; gap: 8px 14px; padding: 8px 10px 12px; color: var(--muted); }}
.legend span {{ display: inline-flex; align-items: center; gap: 6px; }}
.swatch {{ width: 10px; height: 10px; border-radius: 2px; display: inline-block; }}
.chart-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(460px, 1fr)); gap: 14px; align-items: start; }}
@media (max-width: 720px) {{
  .chart-grid {{ grid-template-columns: 1fr; }}
}}
</style>
</head>
<body>
<main>
<h1>Kagesoko ECS Benchmarks</h1>
<p>Characteristic benchmark results are stored per commit and summarized here over time.</p>
<div class="toolbar">
  <label>Filter <input id="filter" type="search" placeholder="benchmark name"></label>
</div>
<section>
  <h2>CPU Time Over Time</h2>
  <div id="cpu-charts" class="chart-grid"></div>
  <div id="cpu"></div>
</section>
<section>
  <h2>Runs</h2>
  <div id="runs"></div>
</section>
</main>
<script>
const history = {history_json};
const filterInput = document.getElementById('filter');
function fmt(value) {{
  if (value === null || value === undefined || Number.isNaN(Number(value))) return '';
  const number = Number(value);
  if (Math.abs(number) >= 1000000) return number.toExponential(3);
  if (Math.abs(number) >= 1000) return number.toFixed(0);
  if (Math.abs(number) >= 10) return number.toFixed(2);
  return number.toFixed(4);
}}
function html(value) {{
  return String(value).replace(/[&<>"']/g, c => ({{'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}}[c]));
}}
function table(headers, rows) {{
  if (!rows.length) return '<div class="empty">No matching data.</div>';
  return '<table><thead><tr>' + headers.map(h => `<th>${{h}}</th>`).join('') +
    '</tr></thead><tbody>' + rows.join('') + '</tbody></table>';
}}
function seriesFrom(metric, filter) {{
  const grouped = new Map();
  const runs = [...history.runs].reverse();
  for (let runIndex = 0; runIndex < runs.length; ++runIndex) {{
    const run = runs[runIndex];
    for (const item of run.cpu || []) {{
      const name = item.name;
      if (filter && !String(name).toLowerCase().includes(filter)) continue;
      const value = Number(item[metric]);
      if (!Number.isFinite(value)) continue;
      if (!grouped.has(name)) grouped.set(name, []);
      grouped.get(name).push({{x: runIndex, value, commit: run.short_commit, started: run.started_at}});
    }}
  }}
  return [...grouped.entries()]
    .map(([name, points]) => ({{name, points}}))
    .filter(series => series.points.length > 0)
    .slice(0, 16);
}}
function chartMarkup(series, ariaLabel, valueLabel) {{
  if (!series.length) return '<div class="empty">No chartable data.</div>';
  const colors = ['#006b5f', '#b33c22', '#3157a4', '#8a5a00', '#6f4ab0', '#26733d', '#a9336b', '#4b6f7c'];
  const width = 640, height = 230, left = 68, right = 18, top = 24, bottom = 42;
  const plotWidth = width - left - right;
  const plotHeight = height - top - bottom;
  const maxRunIndex = Math.max(1, history.runs.length - 1);
  const values = series.flatMap(item => item.points.map(point => point.value));
  let minValue = Math.min(...values);
  let maxValue = Math.max(...values);
  if (minValue === maxValue) {{
    minValue = minValue * 0.95;
    maxValue = maxValue * 1.05 + 1;
  }}
  const yFor = value => top + plotHeight - ((value - minValue) / (maxValue - minValue)) * plotHeight;
  const xFor = index => left + (index / maxRunIndex) * plotWidth;
  const lines = [];
  for (let i = 0; i <= 4; ++i) {{
    const y = top + (plotHeight / 4) * i;
    const value = maxValue - ((maxValue - minValue) / 4) * i;
    lines.push(`<line class="gridline" x1="${{left}}" y1="${{y}}" x2="${{width - right}}" y2="${{y}}"></line>`);
    lines.push(`<text class="axis-label" x="8" y="${{y + 4}}">${{html(fmt(value))}}</text>`);
  }}
  const paths = series.map((item, index) => {{
    const color = colors[index % colors.length];
    const points = item.points.map(point => `${{xFor(point.x).toFixed(1)}},${{yFor(point.value).toFixed(1)}}`).join(' ');
    const dots = item.points.map(point => `<circle cx="${{xFor(point.x).toFixed(1)}}" cy="${{yFor(point.value).toFixed(1)}}" r="3" fill="${{color}}"><title>${{html(item.name)}}\\n${{html(point.commit)}}\\n${{fmt(point.value)}}</title></circle>`).join('');
    return `<polyline points="${{points}}" fill="none" stroke="${{color}}" stroke-width="2"></polyline>${{dots}}`;
  }}).join('');
  const runs = [...history.runs].reverse();
  const first = runs[0]?.short_commit || '';
  const last = runs[runs.length - 1]?.short_commit || '';
  const legend = series.length > 1
    ? '<div class="legend">' + series.map((item, index) => `<span><i class="swatch" style="background:${{colors[index % colors.length]}}"></i>${{html(item.name)}}</span>`).join('') + '</div>'
    : '';
  return `<svg viewBox="0 0 ${{width}} ${{height}}" role="img" aria-label="${{html(ariaLabel)}}">
    ${{lines.join('')}}
    <text class="axis-label" x="${{left}}" y="${{top - 8}}">${{html(valueLabel)}}</text>
    <line class="axis" x1="${{left}}" y1="${{height - bottom}}" x2="${{width - right}}" y2="${{height - bottom}}"></line>
    <line class="axis" x1="${{left}}" y1="${{top}}" x2="${{left}}" y2="${{height - bottom}}"></line>
    <text class="axis-label" x="${{left}}" y="${{height - 16}}">${{html(first)}}</text>
    <text class="axis-label" x="${{width - right - 86}}" y="${{height - 16}}">${{html(last)}}</text>
    <text class="axis-label" x="${{Math.floor(width / 2) - 42}}" y="${{height - 16}}">commit order</text>
    ${{paths}}
  </svg>${{legend}}`;
}}
function renderCpu() {{
  const filter = filterInput.value.toLowerCase();
  const series = seriesFrom('cpu_time', filter);
  const charts = series.map(item => `<div><h3>${{html(item.name)}}</h3><div class="chart">${{chartMarkup([item], item.name + ' CPU time chart', 'CPU time')}}</div></div>`);
  document.getElementById('cpu-charts').innerHTML = charts.length ? charts.join('') : '<div class="empty">No matching CPU benchmarks.</div>';
  const rows = [];
  for (const run of history.runs) {{
    for (const bench of run.cpu || []) {{
      if (filter && !String(bench.name).toLowerCase().includes(filter)) continue;
      rows.push(`<tr><td><a href="${{run.path}}">${{html(run.short_commit)}}</a></td><td>${{html(run.started_at)}}</td><td>${{html(bench.name)}}</td><td class="num">${{fmt(bench.cpu_time)}}</td><td>${{html(bench.time_unit || '')}}</td><td class="num">${{fmt(bench.items_per_second)}}</td></tr>`);
    }}
  }}
  document.getElementById('cpu').innerHTML = table(['Commit', 'Started', 'Benchmark', 'CPU time', 'Unit', 'Items/sec'], rows);
}}
function renderRuns() {{
  const rows = history.runs.map(run => `<tr><td><a href="${{run.path}}">${{html(run.short_commit)}}</a></td><td>${{html(run.ref)}}</td><td>${{html(run.started_at)}}</td><td>${{html(run.subset)}}</td></tr>`);
  document.getElementById('runs').innerHTML = table(['Commit', 'Ref', 'Started', 'Subset'], rows);
}}
function render() {{ renderCpu(); renderRuns(); }}
filterInput.addEventListener('input', render);
render();
</script>
</body>
</html>
""",
        encoding="utf-8",
    )


def main() -> int:
    args = parse_args()
    result = read_json(args.results_dir / "result.json")
    commit = result["commit"]

    benchmark_dir = args.site_dir / "benchmarks"
    commit_dir = benchmark_dir / "commits" / commit
    history_dir = benchmark_dir / "history"
    commit_dir.mkdir(parents=True, exist_ok=True)
    history_dir.mkdir(parents=True, exist_ok=True)

    for child in args.results_dir.iterdir():
        target = commit_dir / child.name
        if child.is_dir():
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(child, target)
        else:
            shutil.copy2(child, target)

    current = compact_run(result)
    runs = merge_history(fetch_existing_history(args.history_url), current)
    history = {"schema_version": 1, "runs": runs}
    (history_dir / "index.json").write_text(json.dumps(history, indent=2) + "\n", encoding="utf-8")
    write_index_html(benchmark_dir / "index.html", history)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
