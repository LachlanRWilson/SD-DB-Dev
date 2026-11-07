# Handoff documentation

Start at [`pdf/00-index.pdf`](pdf/00-index.pdf), then read 01 through 04 in
order. Source is in `typst/`, diagrams in `diagrams/`.

```
docs/
├── typst/      source (.typ) -- edit here, see typst/Makefile
├── diagrams/   handoff-overview.drawio (4 pages) + exported PNGs, see diagrams/README.md
└── pdf/        compiled output (generated -- run `make` in typst/, don't hand-edit)
```

To rebuild after an edit:

```sh
cd typst && make
```

This set is a *handoff guide* -- it orients a new contributor and points
to the much more detailed material that already exists in the repo
(`README.md` at the repo root, and the 29 files under `notes/`) rather
than duplicating it. Read `00-index.pdf` first for the full map.
