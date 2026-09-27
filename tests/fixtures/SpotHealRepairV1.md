# Frozen Spot Heal V1 reference

`SpotHealRepairV1.cpp` is an **unmodified copy of Vulkana's own production
implementation**, not imported research code. It is test-only and never linked
into or installed with the application.

- Source revision: `5d5134f3ceb78ba26ad1ec94890777e80db232d5`
- Original path: `src/core/src/SpotHealRepair.cpp`
- SHA-256: `d9384090955189ee044b756f41e0b42a4f90c7100d897d749d29f9cca2e9e86b`

The equivalence target compiles it against the current platform-neutral input
and result structs, renaming only the public entry point with a compile
definition. Its arithmetic, order, constants, RNG, validity and seven-iteration
schedule stay untouched. New options are ignored by this frozen implementation.
Do not update this file to make a regression disappear. Intentional algorithmic
changes require a separately documented comparison and fresh quality evidence.
