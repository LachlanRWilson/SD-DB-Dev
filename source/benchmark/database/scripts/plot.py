#!/usr/bin/env python3
"""SVG figures from the target database benchmark CSVs (standard library only).

Usage: scripts/plot.py [--date 2026-10-20] [--tag Os] [--out plots] [--inline]

Writes one SVG per figure into --out. Plain files use fixed light colours for
the thesis. --inline writes variants that take their colours from CSS custom
properties (--chart-*) and carry data-tip attributes, for embedding in an HTML
page that supplies the tokens and a tooltip script.
"""
import argparse
import csv
import html
import math
import os

W, H = 680, 380
L, R, T, B = 64, 150, 48, 52

# Validated categorical order (light / dark): blue, orange, aqua
SERIES_LIGHT = ["#2a78d6", "#eb6834", "#1baf7a"]
LIGHT = {"surface": "#fcfcfb", "text": "#0b0b0b", "muted": "#52514e", "grid": "#e6e5e1", "axis": "#b9b8b2"}


class Theme:
    def __init__(self, inline):
        self.inline = inline

    def c(self, name):
        return f"var(--chart-{name})" if self.inline else LIGHT[name]

    def s(self, i):
        return f"var(--chart-s{i + 1})" if self.inline else SERIES_LIGHT[i]


def nice_ticks(hi, n=5):
    """0..hi in round steps (1, 2, 2.5, 5 x 10^k)."""
    if hi <= 0:
        return [0, 1]
    raw = hi / n
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw)
    top = math.ceil(hi / step) * step
    return [round(i * step, 10) for i in range(int(round(top / step)) + 1)]


def fmt_tick(v):
    if v >= 1000:
        return f"{v / 1000:g}K"
    return f"{v:g}"


def esc(s):
    return html.escape(str(s), quote=True)


class Chart:
    def __init__(self, th, title, subtitle, xlabel, ylabel, xmax, ymax, xticks=None):
        self.th, self.out = th, []
        self.xt = xticks or nice_ticks(xmax)
        self.yt = nice_ticks(ymax)
        self.x1, self.y1 = self.xt[-1], self.yt[-1]
        c = th.c
        self.out += [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
            f'font-family="system-ui, -apple-system, Segoe UI, Helvetica, Arial, sans-serif" font-size="12" role="img" aria-label="{esc(title)}">',
            f'<rect width="{W}" height="{H}" fill="{c("surface")}"/>',
            f'<text x="{L}" y="20" font-size="14" font-weight="600" fill="{c("text")}">{esc(title)}</text>',
            f'<text x="{L}" y="36" font-size="11.5" fill="{c("muted")}">{esc(subtitle)}</text>',
        ]
        for v in self.yt:
            y = self.sy(v)
            self.out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" stroke="{c("grid")}" stroke-width="1"/>')
            self.out.append(f'<text x="{L - 8}" y="{y + 4:.1f}" text-anchor="end" fill="{c("muted")}">{fmt_tick(v)}</text>')
        for v in self.xt:
            x = self.sx(v)
            self.out.append(f'<text x="{x:.1f}" y="{H - B + 18}" text-anchor="middle" fill="{c("muted")}">{fmt_tick(v)}</text>')
        self.out.append(f'<line x1="{L}" y1="{H - B}" x2="{W - R}" y2="{H - B}" stroke="{c("axis")}" stroke-width="1"/>')
        self.out.append(f'<text x="{(L + W - R) / 2}" y="{H - 12}" text-anchor="middle" fill="{c("muted")}">{esc(xlabel)}</text>')
        my = (T + H - B) / 2
        self.out.append(f'<text x="16" y="{my}" transform="rotate(-90 16 {my})" text-anchor="middle" fill="{c("muted")}">{esc(ylabel)}</text>')

    def sx(self, v):
        return L + v / self.x1 * (W - L - R)

    def sy(self, v):
        return H - B - v / self.y1 * (H - T - B)

    def marker(self, x, y, color, tip):
        surf = self.th.c("surface")
        attr = f' data-tip="{esc(tip)}" tabindex="0"' if self.th.inline else ""
        self.out.append(f'<g class="pt"{attr}><title>{esc(tip)}</title>'
                        f'<circle cx="{x:.1f}" cy="{y:.1f}" r="11" fill="transparent"/>'
                        f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4.5" fill="{color}" stroke="{surf}" stroke-width="2"/></g>')

    def line(self, pts, i, tips, label=None):
        color = self.th.s(i)
        d = " ".join(f"{self.sx(x):.1f},{self.sy(y):.1f}" for x, y in pts)
        self.out.append(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>')
        for (x, y), tip in zip(pts, tips):
            self.marker(self.sx(x), self.sy(y), color, tip)
        return color

    def text(self, x, y, s, anchor="start", tone="text", size=12, weight=400):
        self.out.append(f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" font-size="{size}" font-weight="{weight}" '
                        f'fill="{self.th.c(tone)}">{esc(s)}</text>')

    def end_labels(self, items):
        """Direct labels right of the plot, nudged apart vertically: (y_px, colour, text)."""
        items = sorted(items)
        placed = []
        for y, color, s in items:
            if placed and y - placed[-1] < 16:
                y = placed[-1] + 16
            placed.append(y)
            self.out.append(f'<line x1="{W - R + 8}" y1="{y - 4:.1f}" x2="{W - R + 22}" y2="{y - 4:.1f}" stroke="{color}" stroke-width="2" stroke-linecap="round"/>')
            self.text(W - R + 27, y, s)

    def legend(self, entries):
        x = L
        for color, s in entries:
            self.out.append(f'<line x1="{x}" y1="{T - 2}" x2="{x + 14}" y2="{T - 2}" stroke="{color}" stroke-width="2" stroke-linecap="round"/>')
            self.text(x + 19, T + 2, s, tone="muted", size=11.5)
            x += 26 + 6.4 * len(s)

    def svg(self):
        return "\n".join(self.out + ["</svg>"])


def load(results, date, tag, bench):
    return list(csv.DictReader(open(os.path.join(results, f"{date}_target_{tag}_{bench}.csv"))))


def ms(r, k="ns_median"):
    return int(r[k]) / 1e6


def fig_mount(th, rows):
    pts = [(int(r["n_contacts"]), int(r["ns_median"]) / 1e9, int(float(r["sectors_read_avg"])))
           for r in rows if r["operation"] == "mount" and r["n_chats"] == "0"]
    ch = Chart(th, "Boot time (database_init) by contacts stored",
               "Median of 5 mounts on the target; no chats or messages", "Contacts stored", "Seconds",
               10000, max(p[1] for p in pts))
    ch.line([(x, y) for x, y, _ in pts], 0, [f"{x:,} contacts: {y:.2f} s, {rd:,} sector reads" for x, y, rd in pts])
    x, y, rd = pts[-1]
    ch.text(ch.sx(x) + 10, ch.sy(y) + 4, f"{y:.1f} s ({rd:,} reads)", weight=600)
    return ch.svg()


def fig_contacts(th, rows):
    ops = [("add", "add"), ("update_name", "update name"), ("remove", "remove")]
    series = {op: [(int(r["n_contacts"]), ms(r), r) for r in rows if r["operation"] == op] for op, _ in ops}
    ymax = max(y for s in series.values() for _, y, _ in s)
    ch = Chart(th, "Contact updates by contacts stored", "Median of 100 calls each; contact_get stays at ~0.9 ms",
               "Contacts stored", "Milliseconds", 10000, ymax)
    labels, legend = [], []
    for i, (op, name) in enumerate(ops):
        s = series[op]
        color = ch.line([(x, y) for x, y, _ in s], i,
                        [f"{name}, {x:,} contacts: {y:.1f} ms, {float(r['sectors_written_avg']):.0f} writes" for x, y, r in s])
        labels.append((ch.sy(s[-1][1]) + 4, color, name))
        legend.append((color, name))
    ch.legend(legend)
    ch.end_labels(labels)
    return ch.svg()


def fig_deep(th, rows):
    ops = [("deep_get_chat20_oldest", "oldest 20"), ("deep_get_chat20_middle", "middle 20"), ("deep_get_chat20_newest", "newest 20")]
    series = {op: [(int(r["msgs_per_chat"]), ms(r), r) for r in rows if r["operation"] == op] for op, _ in ops}
    ymax = max(y for s in series.values() for _, y, _ in s)
    ch = Chart(th, "Reading a page of 20 messages from one chat", "Median of 100 reads, by chat length",
               "Messages in the chat", "Milliseconds", 1000, ymax)
    labels, legend = [], []
    for i, (op, name) in enumerate(ops):
        s = series[op]
        color = ch.line([(x, y) for x, y, _ in s], i,
                        [f"{name}, {x:,}-message chat: {y:.1f} ms, {float(r['sectors_read_avg']):.0f} reads" for x, y, r in s])
        labels.append((ch.sy(s[-1][1]) + 4, color, f"{name}: {s[-1][1]:.0f} ms"))
        legend.append((color, name))
    ch.legend(legend)
    ch.end_labels(labels)
    return ch.svg()


def fig_writes(th, contacts, messages, calls):
    """Median time against sectors written for every timed mutation row."""
    pts = []
    for bench, rows, ops in (("contact", contacts, ("add", "update_name", "update_phone", "remove")),
                             ("message", messages, ("add_msg", "add_msg_new_contact", "deep_add_msg", "wrapped_add_msg")),
                             ("call", calls, ("call_add_known", "call_add_unknown", "wrapped_call_add_known", "wrapped_call_add_unknown"))):
        for r in rows:
            if r["operation"] in ops:
                pts.append((float(r["sectors_written_avg"]), ms(r), f"{bench} {r['operation']}", r))
    for r in messages:
        if r["operation"] == "remove_chat" and int(r["msgs_per_chat"]) <= 10:
            pts.append((float(r["sectors_written_avg"]), ms(r), f"remove_chat ({r['msgs_per_chat']} msg)", r))
    # least squares fit time = a + b * writes
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    b = sum((p[0] - mx) * (p[1] - my) for p in pts) / sum((p[0] - mx) ** 2 for p in pts)
    a = my - b * mx
    xmax = max(p[0] for p in pts)
    ch = Chart(th, "Update time follows the number of sector writes",
               f"Median per row of every add / update / remove benchmark ({n} rows); line: least-squares fit",
               "Sectors written per call", "Milliseconds", xmax, max(p[1] for p in pts))
    x0, x1 = 0, ch.x1
    ch.out.append(f'<line x1="{ch.sx(x0):.1f}" y1="{ch.sy(max(a, 0)):.1f}" x2="{ch.sx(x1):.1f}" y2="{ch.sy(a + b * x1):.1f}" '
                  f'stroke="{th.c("axis")}" stroke-width="1.5"/>')
    xm = x1 * 0.42
    ch.text(ch.sx(xm) - 8, ch.sy(a + b * xm) - 10, f"fit: {a:.1f} ms + {b:.1f} ms per write", anchor="end", weight=600)
    color = th.s(0)
    for x, y, name, r in pts:
        ch.marker(ch.sx(x), ch.sy(y), color,
                  f"{name}: {y:.1f} ms, {x:.1f} writes, {float(r['sectors_read_avg']):.1f} reads")
    return ch.svg(), a, b


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default="2026-10-20")
    ap.add_argument("--tag", default="Os")
    ap.add_argument("--out", default=os.path.join(here, "plots"))
    ap.add_argument("--inline", action="store_true")
    a = ap.parse_args()
    res = os.path.join(here, "results")
    th = Theme(a.inline)
    os.makedirs(a.out, exist_ok=True)
    contacts = load(res, a.date, a.tag, "contacts")
    messages = load(res, a.date, a.tag, "messages")
    calls = load(res, a.date, a.tag, "calls")
    init = load(res, a.date, a.tag, "init")
    writes_svg, fa, fb = fig_writes(th, contacts, messages, calls)
    figs = {"mount_time": fig_mount(th, init), "contact_updates": fig_contacts(th, contacts),
            "chat_page_depth": fig_deep(th, messages), "time_vs_writes": writes_svg}
    suffix = "_inline" if a.inline else ""
    for name, svg in figs.items():
        path = os.path.join(a.out, f"{a.date}_target_{a.tag}_{name}{suffix}.svg")
        open(path, "w").write(svg)
        print(path)
    print(f"fit: {fa:.2f} ms + {fb:.2f} ms per write")


if __name__ == "__main__":
    main()
