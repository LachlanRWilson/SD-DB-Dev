#import "common.typ": *

#show: doc.with(
  "Handoff Guide",
  subtitle: "Start here",
)

= What this is

UniQOS DB is an embedded contacts/messages/calls database for the STM32H723
(Cortex-M7, FreeRTOS), designed to manage up to *10,000 contacts* and roughly
twice as many messages on an SD card, with power-fail-safe writes and a RAM
footprint small enough to share the chip with the rest of the UniQOS phone
firmware. The database, its on-hardware demos and unit tests, two benchmark
suites, and a set of offline Python design studies all live in this one
repository.

This `docs/` set is a *handoff guide*: it orients a new contributor and links
out to the much more detailed material that already exists in the repository
(`README.md` and the 29 files under `notes/`) rather than repeating it.
Read this set first, then go deeper via the pointers in each chapter.

#pointer[
  *Already in the repo and worth knowing about before you read further:*
  - `README.md` -- the original architecture write-up (relational model, SD
    layout, hash table reconstruction, rollback journal, tombstoning), with
    diagrams.
  - `notes/` -- 29 markdown files: design rationale, debugging postmortems,
    build options, demo/CLI usage, and one file per GTest suite under
    `notes/unit_tests/`.
  - `diagrams/UniQOS DB Architecture.drawio` -- 25 pages covering the DB's
    internal data structures and flows in detail. This handoff set's own
    diagrams (`docs/diagrams/handoff-overview.drawio`) are a separate,
    smaller set covering what that file doesn't: the repo layout, the build
    option matrix, the benchmark pipeline, and current project status.
]

= How to read this set

/ 01 -- Architecture Overview: System layers and the module responsibility
  table for `source/app`. Read this first if you're new to the codebase.
/ 02 -- Build, Test & Flash: How to configure, build, test and flash both
  the host and STM32 targets, and how to read test results off the board.
/ 03 -- Benchmarking & Design Evidence: How the hashing and collision
  design decisions were validated, from the offline Python studies through
  to the on-target benchmarks.
/ 04 -- Project Status & Next Steps: What's stable, what's currently
  mid-flight in the working tree, and the known open items worth picking
  up next.

= Repo map

#fig("/diagrams/png/repo-module-map.png", [
  Where each directory fits: design studies inform the DB core, which is
  verified by the test suite and measured by the benchmarks, and runs on
  the STM32 firmware (or standalone via the host CLI).
])

= At a glance

#table(
  columns: (auto, 1fr),
  stroke: 0.5pt + rule-colour,
  inset: 6pt,
  [*Target MCU*], [STM32H723ZGTx (Cortex-M7), Nucleo-based board],
  [*Design capacity*], [10,000 contacts, ~2x that in messages],
  [*Hash table*], [14,293 slots, DJB2 hashing, double hashing + tombstoning, 70% load cap],
  [*Sector size*], [512 B -- 6 contacts/sector (81 B each), 2 messages/sector (164 B each)],
  [*RAM usage*], [200.092 KB / 320 KB of RAM\_D1 (AXI SRAM) for the hash table + name index + free-list stacks],
  [*Test suite*], [336 GTest tests across 14 suites, run host-side via `ctest`],
  [*Build system*], [CMake; separate `build-host/` (GTest) and `build/` (STM32, Ninja + toolchain file) trees],
)
