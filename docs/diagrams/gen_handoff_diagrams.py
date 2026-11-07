"""Generates handoff-overview.drawio: a 4-page multi-page diagram for the docs/ handoff set.

Run from anywhere: `python3 gen_handoff_diagrams.py`. Writes ../diagrams/handoff-overview.drawio
(relative to this script, i.e. docs/diagrams/handoff-overview.drawio).
"""
from html import escape
from pathlib import Path

OUT = Path(__file__).resolve().parent / "handoff-overview.drawio"

FONT = "Helvetica"
MONO = "'JetBrains Mono','DejaVu Sans Mono',monospace"

GREEN = "#d5e8d4"
GREEN_S = "#82b366"
YELLOW = "#fff2cc"
YELLOW_S = "#d6b656"
RED = "#f8cecc"
RED_S = "#b85450"
BLUE = "#dae8fc"
BLUE_S = "#6c8ebf"
PURPLE = "#e1d5e7"
PURPLE_S = "#9673a6"
GREY = "#f5f5f5"
GREY_S = "#666666"

_uid = [0]


def nid(prefix):
    _uid[0] += 1
    return f"{prefix}{_uid[0]}"


def box(cells, x, y, w, h, label, fill, stroke, *, rounded=True, font_size=12, bold=False, mono=False, align="center"):
    bid = nid("n")
    style = (
        f"rounded={1 if rounded else 0};whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};"
        f"fontFamily={MONO if mono else FONT};fontSize={font_size};align={align};verticalAlign=middle;"
        f"arcSize=8;{'fontStyle=1;' if bold else ''}"
    )
    cells.append(
        f'<mxCell id="{bid}" value="{escape(label, quote=True)}" style="{style}" vertex="1" parent="1">'
        f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>'
    )
    return bid


def edge(cells, src, dst, label="", *, dashed=False, color="#666666", exit=None, entry=None):
    eid = nid("e")
    exit_s = f"exitX={exit[0]};exitY={exit[1]};exitDx=0;exitDy=0;" if exit else ""
    entry_s = f"entryX={entry[0]};entryY={entry[1]};entryDx=0;entryDy=0;" if entry else ""
    style = (
        f"edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;endArrow=block;elbow=vertical;"
        f"strokeColor={color};fontFamily={FONT};fontSize=10;{exit_s}{entry_s}{'dashed=1;' if dashed else ''}"
    )
    cells.append(
        f'<mxCell id="{eid}" value="{escape(label, quote=True)}" style="{style}" edge="1" parent="1" source="{src}" target="{dst}">'
        f"<mxGeometry relative=\"1\" as=\"geometry\"/></mxCell>"
    )
    return eid


def page(name, width, height, builder):
    cells = []
    builder(cells)
    body = "\n".join("        " + c for c in cells)
    return f'''  <diagram id="{name.replace(" ", "_")}" name="{escape(name, quote=True)}">
    <mxGraphModel dx="1400" dy="800" grid="1" gridSize="10" guides="1" tooltips="1" connect="1" arrows="1" fold="1" page="1" pageScale="1" pageWidth="{width}" pageHeight="{height}" math="0" shadow="0">
      <root>
        <mxCell id="0"/>
        <mxCell id="1" parent="0"/>
{body}
      </root>
    </mxGraphModel>
  </diagram>'''


# ---------------------------------------------------------------------------
# Page 1: Repo & Module Map
# ---------------------------------------------------------------------------
def build_page1(cells):
    title = box(cells, 40, 20, 1000, 40, "UniQOS DB -- Repo & Module Map", GREY, GREY_S, rounded=False, font_size=18, bold=True)

    notes = box(cells, 40, 100, 220, 70, "notes/\n29 design + debug docs\n(start here for depth)", PURPLE, PURPLE_S)
    docs = box(cells, 300, 100, 220, 70, "docs/\nthis handoff set", PURPLE, PURPLE_S)

    pysim = box(cells, 40, 240, 220, 90, "pythonSim/\nhash_selection_eval.py\ncollision_selection_eval.py\n(offline design studies)", BLUE, BLUE_S)

    app = box(cells, 370, 240, 260, 110, "source/app/\nDB core: database.c, hash_table.c,\nsorted_array.c, chat_order.c,\njournal.c, storage.c + backends,\nring_buffer.c, usage_bitmap.c, ...", GREEN, GREEN_S, bold=True)

    bench = box(cells, 740, 240, 260, 90, "source/benchmark/\ndatabase/ (API timing)\nname_index/ (array vs AVL tree)", BLUE, BLUE_S)

    tools = box(cells, 1060, 240, 200, 90, "tools/\nstruct_layout.py\ngen_lcd_font.py", GREY, GREY_S)

    demo = box(cells, 290, 440, 190, 80, "source/demo/db_cli/\nhost CLI over database.c\n(no hardware needed)", GREY, GREY_S, font_size=11)

    tests = box(cells, 510, 440, 190, 80, "tests/\n336 GTest tests, 14 suites\n(see notes/unit_tests/)", GREEN, GREEN_S, font_size=11)

    fw = box(cells, 730, 440, 190, 80, "firmware/ + platform/\nSTM32CubeMX project +\nhost/target OS abstraction", YELLOW, YELLOW_S, font_size=11)

    edge(cells, pysim, app, "informs design")
    edge(cells, app, bench, "measured by", exit=(1, 0.3), entry=(0, 0.3))
    edge(cells, app, demo, "host-only via", exit=(0.25, 1), entry=(0.5, 0))
    edge(cells, app, tests, "verified by", exit=(0.5, 1), entry=(0.5, 0))
    edge(cells, app, fw, "runs on", exit=(0.85, 1), entry=(0.3, 0))


# ---------------------------------------------------------------------------
# Page 2: Build Option Flow
# ---------------------------------------------------------------------------
def build_page2(cells):
    box(cells, 40, 20, 1040, 40, "Build Option Flow (main.c, firmware build) -- see notes/build_options.md", GREY, GREY_S, rounded=False, font_size=18, bold=True)

    steps = [
        ("SD_BUS_TEST=ON?", "sd_bus_test.c\nSD bus diagnostic"),
        ("PERSIST_DEMO=ON?", "persist_demo.c\nwrite/read persistence demo"),
        ("MH_TEST=ON?", "mh_main.c\nmessage-history HW unit tests"),
        ("MH_DEMO=ON?", "mh_demo.c\nmessage-history demo"),
        ("SA_TEST=ON?", "sa_main.c\nsorted-array HW unit tests"),
        ("DATABASE_TEST=ON?", "database_main.c\nfull DB API HW unit tests"),
        ("DB_DEMO=ON?", "db_demo.c\nwhole-database demo (13 sections)"),
        ("NAME_INDEX_BENCH=ON?", "name_index_bench\narray vs AVL benchmark"),
    ]
    x, y, w, h, gap = 40, 90, 220, 55, 85
    prev_no_port = None
    for i, (cond, action) in enumerate(steps):
        cy = y + i * gap
        dcell = box(cells, x, cy, w, h, cond, YELLOW, YELLOW_S)
        acell = box(cells, x + w + 60, cy, 300, h, action, BLUE, BLUE_S)
        edge(cells, dcell, acell, "yes")
        if prev_no_port is not None:
            edge(cells, prev_no_port, dcell, "no")
        prev_no_port = dcell
    default = box(cells, x, y + len(steps) * gap, w, h, "(none set)", GREEN, GREEN_S)
    default_action = box(cells, x + w + 60, y + len(steps) * gap, 300, h, "db_main.c\ndefault hardware DB unit tests", GREEN, GREEN_S, bold=True)
    edge(cells, prev_no_port, default, "no")
    edge(cells, default, default_action, "")

    targets = box(cells, 760, 90, 300, 150,
                   "CMake targets (cmake/Stm32Tools.cmake)\n\nflash   -- STM32_Programmer_CLI -w ... -v -rst\nserver  -- openocd (ST-Link) in new terminal\ndebug   -- server + gdb-multiarch attach",
                   GREY, GREY_S, align="left", font_size=11)
    persist = box(cells, 760, 270, 300, 110,
                  "Persistence check workflow:\n1. flash DB_TEST_WRITE=ON\n2. flash DB_TEST_WRITE=OFF\n3. power-cycle (unplug USB)\n-> LCD shows 1/1 PASSED",
                  PURPLE, PURPLE_S, align="left", font_size=11)


# ---------------------------------------------------------------------------
# Page 3: Benchmark & Design-Evidence Pipeline
# ---------------------------------------------------------------------------
def build_page3(cells):
    box(cells, 40, 20, 1160, 40, "Benchmark & Design-Evidence Pipeline", GREY, GREY_S, rounded=False, font_size=18, bold=True)

    sim = box(cells, 40, 110, 240, 110,
              "pythonSim/\nhash_selection_eval.py\ncollision_selection_eval.py\n(DJB2 vs FNV/SDBM/Murmur/CRC32;\ndouble hashing vs cuckoo)", BLUE, BLUE_S, align="left", font_size=11)
    results = box(cells, 340, 110, 220, 110,
                   "pythonSim/results/\nhash_selection/\nCSVs + PNG/SVG figures\n(load factor, probes, uniformity)", GREY, GREY_S, align="left", font_size=11)
    decision = box(cells, 620, 110, 240, 110,
                    "Design decision (shipped)\nDJB2 hash + double hashing\nwith tombstoning\nsource/app/src/hash_table.c", GREEN, GREEN_S, bold=True, align="left", font_size=11)
    name_idx = box(cells, 920, 20, 260, 90,
                   "source/benchmark/name_index/\nsorted array vs AVL tree\n(sorted array wins: fits RAM, faster)", BLUE, BLUE_S, align="left", font_size=11)

    db_bench = box(cells, 620, 280, 240, 110,
                   "source/benchmark/database/\nbench_contacts/messages/calls/init.c\nhost (-O3) + target (-Os) runs", BLUE, BLUE_S, align="left", font_size=11)
    reports = box(cells, 920, 280, 260, 110,
                  "Reports\nsource/benchmark/*/RESULTS.md\nreport/dbms-benchmark.html\nplots/*.svg", GREEN, GREEN_S, align="left", font_size=11)

    edge(cells, sim, results, "writes")
    edge(cells, results, decision, "justifies")
    edge(cells, decision, db_bench, "measured by")
    edge(cells, decision, name_idx, "validated by")
    edge(cells, db_bench, reports, "reported in")
    edge(cells, name_idx, reports, "reported in")


# ---------------------------------------------------------------------------
# Page 4: Project Status Map
# ---------------------------------------------------------------------------
def build_page4(cells):
    box(cells, 40, 20, 1080, 40, "Project Status Map (snapshot -- see docs/typst/04-status-and-next-steps.typ)", GREY, GREY_S, rounded=False, font_size=18, bold=True)

    box(cells, 40, 90, 300, 30, "STABLE / COMMITTED", GREEN, GREEN_S, rounded=False, bold=True)
    stable_items = [
        "database.c / hash_table.c API (336 tests passing)",
        "journal.c power-fail rollback",
        "ring_buffer.c + message/call history",
        "storage.c per-sector CRC-32 trailer",
        "sorted_array.c name index (benchmarked vs AVL tree)",
    ]
    for i, t in enumerate(stable_items):
        box(cells, 40, 130 + i * 45, 300, 40, t, GREEN, GREEN_S, font_size=10, align="left")

    box(cells, 400, 90, 300, 30, "IN PROGRESS / UNCOMMITTED", YELLOW, YELLOW_S, rounded=False, bold=True)
    wip_items = [
        "mem_layout.h -- comment reformatting only",
        "UniQOS DB Architecture.drawio -- modified",
        "pythonSim/results/hash_selection/ -- new, untracked",
        "pythonSim/{hash,collision}_selection_eval.py -- untracked",
        "demo.img -- regenerated demo card image",
    ]
    for i, t in enumerate(wip_items):
        box(cells, 400, 130 + i * 45, 300, 40, t, YELLOW, YELLOW_S, font_size=10, align="left")

    box(cells, 760, 90, 360, 30, "KNOWN OPEN ITEMS", RED, RED_S, rounded=False, bold=True)
    open_items = [
        "journal_add() / check_usage_bit(): no bounds check on index",
        "free_list_free(): NULL self dereferenced (no guard)",
        "hash_clear() uses global HASH_TABLE_SIZE, not table->size",
        "hash_remove_contact() leaks a contact's message sectors",
        "hash_insert_message(): phantom contact on allocator exhaustion",
        "see notes/unit_tests/README.md 'Recommended additional tests'",
    ]
    for i, t in enumerate(open_items):
        box(cells, 760, 130 + i * 45, 360, 40, t, RED, RED_S, font_size=10, align="left")


PAGES = [
    ("Repo & Module Map", 1300, 580, build_page1),
    ("Build Option Flow", 1240, 820, build_page2),
    ("Benchmark & Design-Evidence Pipeline", 1240, 420, build_page3),
    ("Project Status Map", 1160, 450, build_page4),
]

diagram_xml = "\n".join(page(name, w, h, b) for name, w, h, b in PAGES)

xml = f'''<mxfile host="Electron" type="device">
{diagram_xml}
</mxfile>
'''

OUT.write_text(xml)
print("wrote", OUT)
