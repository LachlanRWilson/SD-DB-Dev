#import "common.typ": *

#show: doc.with(
  "Project Status & Next Steps",
  subtitle: "Snapshot of the working tree and what to pick up next",
  doc-number: "04",
)

= Snapshot

#fig("/diagrams/png/project-status-map.png", [
  What's stable, what's mid-flight in the working tree, and the known open
  items -- condensed from the sections below.
])

= Working tree, as of this writing

#table(
  columns: (auto, 1fr, 1fr),
  stroke: 0.5pt + rule-colour,
  inset: 6pt,
  [*Status*], [*Path*], [*What changed*],
  [#wip], [`source/app/include/mem_layout.h`], [Comment style only -- replaced a boxed banner comment with a single-line one. No functional change in the diff seen at generation time.],
  [#wip], [`diagrams/UniQOS DB Architecture.drawio`], [Modified, uncommitted. 25 pages. Not redrawn by this handoff set; its content wasn't diffed here -- check `git diff` before assuming what changed.],
  [#wip], [`demo.img`], [Regenerated demo SD-card image (binary, used by `source/demo/db_cli`).],
  [#open-item], [`docs/`], [This handoff set -- new, untracked at the time of writing.],
  [#open-item], [`pythonSim/hash_selection_eval.py`, `collision_selection_eval.py`], [New design-evidence scripts, untracked -- see *03 -- Benchmarking & Design Evidence*.],
  [#open-item], [`pythonSim/results/hash_selection/`], [Output of the two scripts above: ~25 CSV/PNG/SVG files, untracked.],
)

#pointer[
  None of the untracked `pythonSim` work or this `docs/` set has been
  committed. If you pick this project up, deciding whether/how to commit
  them is one of the first calls to make -- they're real, finished design
  evidence, not scratch output.
]

= Known unresolved bug: code 7 (`T_MESSAGE_FIND`)

`notes/code7_message_reconstruction_bug.md` documents a GDB session
against real hardware investigating a persistent code-7 failure in
`db_run_write()`. Status as recorded there:

- A `message.c` bitmap offset bug (§1) -- *fixed, confirmed working*.
- A `hash_reconstruct_message()` scan-bounds bug (§2) -- *real bug, a
  partial fix was attempted and reverted*. Even with that fix applied,
  code 7 persisted and `insert_message_from_sector` was never called, so
  *there is at least one more bug in this path not yet found*.

This is the single most concrete lead for whoever picks this up next --
read that note before touching message reconstruction.

= Stable / committed

The core API (`database.c`, `hash_table.c`) passes all 336 host tests.
`journal.c` rollback, `ring_buffer.c` (message/call history),
`storage.c`'s per-sector CRC-32 trailer, and `sorted_array.c` (chosen
over an AVL tree with benchmark evidence) are all exercised by the test
suite and considered settled.

= Open items from the test-suite review

`notes/unit_tests/README.md` keeps a prioritised "Recommended additional
tests" list; condensed here, see that file for full detail and code
references:

+ *No bounds check* on `journal_add()` / `check_usage_bit()` for an
  out-of-range sector index -- `journal_add(&journal, JRNL_CONTACT,
  UINT16_MAX, content)` segfaults today.
+ `free_list_free()` dereferences a `NULL self` with no guard (unlike
  `free_list_allocate()`, which checks).
+ `hash_clear()` uses the global `HASH_TABLE_SIZE` instead of
  `table->size` -- a table initialised smaller than the default with a
  minimally-sized backing array would overrun on clear.
+ Two characterised-but-unresolved design gaps, pinned down by tests but
  not yet decided on:
  - `hash_remove_contact()` leaks a contact's message sectors if it has
    any.
  - `hash_insert_message()` leaves a phantom empty contact behind when
    message-sector allocation fails right after a brand-new contact was
    committed.
+ Ring buffer iterator off-by-one decisions (upper limit `occupancy` vs.
  `occupancy - 1`; whether `move_prev` wraps like `move_next`).
+ No death test / known-vector coverage for `crc.c` against published
  CRC-32 test vectors.
+ Four `.cpp` files under `tests/` aren't wired into `tests/CMakeLists.txt`
  and don't run: `test_hash_table_messages.cpp`, `test_message_extent.cpp`,
  `test_my_logic.cpp`, `test_database.cpp` (empty) -- worth deciding
  whether to revive, rewrite, or delete.

= Suggested order of attack

+ Read `notes/code7_message_reconstruction_bug.md` and resolve the
  remaining message-reconstruction bug -- it's the only item here
  blocking correctness rather than coverage.
+ Decide what to do with the uncommitted `pythonSim` design-evidence
  scripts and this `docs/` set (commit, or fold into the thesis write-up
  directly).
+ Work through the bounds-check / NULL-guard items (#1--3 above) -- small,
  well-isolated fixes with an existing characterisation test pattern to
  follow.
+ Decide the two characterised design gaps (contact message-sector leak,
  phantom contact on allocator exhaustion) -- these need a design call,
  not just a test.
+ Revisit the ring buffer off-by-one questions and the four orphaned test
  files.
