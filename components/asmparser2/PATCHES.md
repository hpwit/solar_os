# Vendored asmparser2 (ESPLiveScript 2)

Source: [hpwit/new-parser](https://github.com/hpwit/new-parser)
at the commit in `UPSTREAM_COMMIT` (only `src/` is vendored). `executeAsTask()` with
parameters, first added here, is upstream since 456ecb5.

Changes:

- `visitnode.h`/`visitnode.cpp`: global `_compare` renamed `asm_compare_positions`.
  ESP-IDF's Bluetooth tinycrypt (`bt/common/tinycrypt/src/utils.c`) also defines a
  global `_compare`, so linking both failed with a multiple-definition error.
