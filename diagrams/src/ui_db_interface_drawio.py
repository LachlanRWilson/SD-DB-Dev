"""Generates ../ui_db_interface.drawio : the poster's "UI Database Interface" table as an editable draw.io table."""
from html import escape
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "ui_db_interface.drawio"

PURPLE = "#51247A"
RULE = "#CFC8D8"
MONO = "'JetBrains Mono','JetBrainsMono Nerd Font Mono','DejaVu Sans Mono',monospace"
FONT = "Helvetica"

# request colour = argument struct (same as the poster / db_task page)
ARGS = {"add": "#dae8fc", "phone": "#d5e8d4", "upd": "#ffe6cc", "seek": "#f8cecc",
        "range": "#fff2cc", "chat": "#b0e3e6", "madd": "#e1d5e7"}
# structure chips (same fills as the poster)
CHIP = {"hash": "#efe9f5", "name idx": "#f8ecc8", "FLS": "#efe9f5", "bitmap": "#efe9f5",
        "journal": "#f6d9d5", "extents": "#efe9f5", "ring": "#f8ecc8", "chat order": "#f8ecc8"}

ROWS = [
    ("CONTACT_ADD", "add", "Save a number", ["hash", "name idx", "FLS", "bitmap", "journal"],
     "Insert, O(n) index shift ≤ 0.5 ms, journaled"),
    ("CONTACT_GET", "phone", "Caller name", ["hash"],
     "≈ 1.7 probes + <b style=\"color:#51247A\">1 sector read</b> ≈ 0.7 ms"),
    ("CONTACT_UPDATE", "upd", "Edit a contact", ["hash", "name idx", "chat order", "journal"],
     "Index remove + reinsert; journaled"),
    ("CONTACT_REMOVE", "phone", "Delete a contact", ["hash", "name idx", "chat order", "FLS", "bitmap", "journal"],
     "Tombstone, shift left, <code>push</code> sector, clear bit"),
    ("CONTACT_LIST", "range", "Contacts page", ["name idx", "hash"],
     "O(1) rank + ≤ 9 sector reads ≤ 6.5 ms"),
    ("CONTACT_SEEK", "seek", "Jump to a letter", ["name idx"],
     "O(log n) binary search in RAM → <code>rank</code>"),
    ("MESSAGE_ADD", "madd", "SMS sent / received", ["hash", "extents", "FLS", "ring", "chat order", "journal"],
     "Append to head extent, ring <code>put</code>, chat to front, journaled"),
    ("MESSAGE_GET_CHAT", "chat", "Open a chat", ["hash", "extents"],
     "⌈N/3⌉ sector reads, newest first"),
    ("MESSAGE_REMOVE_CHAT", "phone", "Delete a chat", ["hash", "extents", "FLS", "chat order", "journal"],
     "Walk <code>prev</code>, <code>push</code> each sector"),
    ("MESSAGE_RECENT", "range", "SMS inbox", ["ring"],
     "Walk back from head: 1 read per 2 SMS"),
    ("CHAT_LIST", "range", "Messages page", ["chat order", "hash", "extents"],
     "O(1) rank, then 2 reads per chat (contact + latest SMS)"),
    ("CALL_ADD", "phone", "Call ends", ["hash", "ring", "journal"],
     "Name lookup, ring <code>put</code>, journaled"),
    ("CALL_RECENT", "range", "Recent calls", ["ring"],
     "Walk back from head: 1 read per 5 calls"),
]

COLS = [("Request <span style=\"font-family:%s;font-size:10px\">DB_REQ_*</span>" % MONO, 190),
        ("Triggered by", 150), ("Structures", 360), ("Work and cost", 430)]
HEAD_H, ROW_H = 26, 26
W = sum(w for _, w in COLS)
H = HEAD_H + ROW_H * len(ROWS)


def req_html(name, a):
    return f'<span style="font-family:{MONO};font-weight:bold;font-size:11px;">{name}</span>'


def chips_html(items):
    return "&nbsp;".join(f'<span style="background-color:{CHIP[i]};">&nbsp;{i}&nbsp;</span>' for i in items)


def code(s):
    return s.replace("<code>", f'<span style="font-family:{MONO};font-size:10px;">').replace("</code>", "</span>")


cells = []
uid = [0]


def nid(p):
    uid[0] += 1
    return f"{p}{uid[0]}"


TABLE = "uidb-table"
cells.append(
    f'<mxCell id="{TABLE}" value="" style="shape=table;startSize=0;container=1;collapsible=0;childLayout=tableLayout;'
    f'fixedRows=1;rowLines=1;columnLines=0;fontStyle=0;strokeColor={RULE};fillColor=#FFFFFF;fontFamily={FONT};'
    f'top=0;left=0;right=0;bottom=1;" vertex="1" parent="1">'
    f'<mxGeometry x="40" y="40" width="{W}" height="{H}" as="geometry"/></mxCell>')


def row(values, y, h, header=False):
    rid = nid("uidb-row")
    cells.append(
        f'<mxCell id="{rid}" value="" style="shape=tableRow;horizontal=0;startSize=0;swimlaneHead=0;swimlaneBody=0;'
        f'strokeColor=inherit;top=0;left=0;bottom=0;right=0;collapsible=0;dropTarget=0;fillColor=none;'
        f'points=[[0,0.5],[1,0.5]];portConstraint=eastwest;" vertex="1" parent="{TABLE}">'
        f'<mxGeometry y="{y}" width="{W}" height="{h}" as="geometry"/></mxCell>')
    x = 0
    for (val, (_, w)) in zip(values, COLS):
        if header:
            look = f"fillColor={PURPLE};fontColor=#FFFFFF;fontStyle=1;"
        else:
            look = "fillColor=none;fontColor=#1F1D24;"
        cells.append(
            f'<mxCell id="{nid("uidb-cell")}" value="{escape(val, quote=True)}" style="shape=partialRectangle;html=1;'
            f'whiteSpace=wrap;connectable=0;strokeColor=inherit;overflow=hidden;{look}top=0;left=0;bottom=0;right=0;'
            f'pointerEvents=1;align=left;verticalAlign=middle;spacingLeft=6;fontSize=12;fontFamily={FONT};" '
            f'vertex="1" parent="{rid}"><mxGeometry x="{x}" width="{w}" height="{h}" as="geometry">'
            f'<mxRectangle width="{w}" height="{h}" as="alternateBounds"/></mxGeometry></mxCell>')
        x += w


row([c for c, _ in COLS], 0, HEAD_H, header=True)
for i, (name, a, trig, structs, work) in enumerate(ROWS):
    row([req_html(name, a), trig, chips_html(structs), code(work)], HEAD_H + i * ROW_H, ROW_H)

xml = f'''<mxfile host="Electron" type="device">
  <diagram id="ui-db-interface" name="ui_db_interface">
    <mxGraphModel dx="1400" dy="800" grid="1" gridSize="10" guides="1" tooltips="1" connect="1" arrows="1" fold="1" page="1" pageScale="1" pageWidth="1240" pageHeight="500" math="0" shadow="0">
      <root>
        <mxCell id="0"/>
        <mxCell id="1" parent="0"/>
        {chr(10).join("        " + c for c in cells)}
      </root>
    </mxGraphModel>
  </diagram>
</mxfile>
'''
OUT.write_text(xml)
print("wrote", OUT)
