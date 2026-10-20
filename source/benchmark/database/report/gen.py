#!/usr/bin/env python3
"""Build dbms-benchmark.html from template.html, the target result CSVs and the
*_inline.svg figures in the output directory. Run through report/build.sh."""
import csv, html, sys
R = sys.argv[1]
OUT = sys.argv[2]
def rows(name): return list(csv.DictReader(open(f"{R}/2026-10-20_target_Os_{name}.csv")))
def t(ns):
    ns = int(ns)
    if ns >= 1_000_000_000: return f"{ns/1e9:.2f} s"
    if ns >= 1_000_000: return f"{ns/1e6:.2f} ms"
    if ns >= 1_000: return f"{ns/1e3:.1f} µs"
    return f"{ns} ns"
def io(r): return f"{float(r['sectors_read_avg']):.1f} / {float(r['sectors_written_avg']):.1f}"
e = html.escape

def contacts_tables():
    rs = rows("contacts")
    ops = list(dict.fromkeys(r["operation"] for r in rs))
    ns = sorted({int(r["n_contacts"]) for r in rs})
    cell = {(r["operation"], int(r["n_contacts"])): r for r in rs}
    head = "".join(f"<th>{n:,}</th>" for n in ns)
    def table(fn, cap):
        body = ""
        for op in ops:
            body += f"<tr><th scope='row'><code>{e(op)}</code></th>" + "".join(
                f"<td>{fn(cell[(op,n)]) if (op,n) in cell else '–'}</td>" for n in ns) + "</tr>"
        return f"<div class='tw'><table><caption>{cap}</caption><thead><tr><th>operation</th>{head}</tr></thead><tbody>{body}</tbody></table></div>"
    return (table(lambda r: t(r["ns_median"]), "Median time per call, by contacts stored (N)") +
            table(lambda r: t(r["ns_p99"]), "p99 time per call") +
            table(io, "Sectors read / written per call"))

def long_table(name, cap, cols):
    rs = rows(name)
    hdr = "".join(f"<th>{c[0]}</th>" for c in cols)
    body = ""
    for r in rs:
        tds = ""
        for _, f in cols:
            v = f(r)
            tds += f"<td>{v}</td>"
        body += f"<tr><th scope='row'><code>{e(r['operation'])}</code></th>{tds}</tr>"
    return f"<div class='tw'><table><caption>{cap}</caption><thead><tr><th>operation</th>{hdr}</tr></thead><tbody>{body}</tbody></table></div>"

num = lambda k: (lambda r: f"{int(r[k]):,}" if r[k] != "0" else "–")
std = [("median", lambda r: t(r["ns_median"])), ("p99", lambda r: t(r["ns_p99"])), ("reps", lambda r: r["reps"]), ("sectors r / w", io)]
msg = long_table("messages", "Every row with the database state it ran against",
    [("chats", num("n_chats")), ("msgs in chat", num("msgs_per_chat")), ("history", num("history_len"))] + std)
calls = long_table("calls", "Call history filled to each checkpoint; <code>wrapped_</code> rows ran after the 25,000-entry ring wrapped",
    [("calls in history", num("history_len"))] + std)
init = long_table("init", "Format once, then <code>database_init()</code> at growing states",
    [("contacts", num("n_contacts")), ("chats", num("n_chats")), ("messages", num("history_len"))] + std)

page = open(f"{OUT}/template.html").read()
page = page.replace("{{CONTACTS}}", contacts_tables()).replace("{{MESSAGES}}", msg).replace("{{CALLS}}", calls).replace("{{INIT}}", init)
import glob, os, re
for f in glob.glob(f"{OUT}/*_inline.svg"):
    key = re.sub(r"^\d{4}-\d{2}-\d{2}_target_Os_|_inline\.svg$", "", os.path.basename(f))
    svg = open(f).read().replace(' width="680" height="380"', "")
    page = page.replace("{{FIG_" + key + "}}", svg)
open(f"{OUT}/dbms-benchmark.html", "w").write(page)
print("ok")
