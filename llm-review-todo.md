# UEFI Manager Repo Review and LLM TODO Handoff

## Scope Reviewed
- `src/*.cpp`, `src/*.h`
- `scripts/helper`, `scripts/uefimanager-lib`
- `tests/test_utils.cpp`, `tests/test_helper.sh`
- `CMakeLists.txt`

## Validation Run
- Build: `cmake -G Ninja -S . -B /tmp/uefi-manager-build-check -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/uefi-manager-build-check --parallel`
- Tests: `ctest --test-dir /tmp/uefi-manager-build-check --output-on-failure`
- Helper tests: `bash tests/test_helper.sh`
- Result: passing in this environment.

## Findings (Ordered by Severity)

### 1) High: `uefimanager-lib` command dispatcher typo breaks checkfile write path
- File: `scripts/uefimanager-lib:11`
- File: `scripts/uefimanager-lib:21`
- File: `scripts/uefimanager-lib:22`
- Details:
  - Function is defined as `write_checkfile`.
  - `case` matches `write_chkfile`.
  - Case calls `write_chkfile` (undefined).
  - Current caller uses `write_checkfile` (`src/mainwindow.cpp:780`), so this path is effectively broken/silent.
- Impact:
  - Frugal prompt suppression marker (`/etc/uefi-stub-installer.chk`) may never be written reliably.

### 2) High: Shell-style quoting still present in argv-based `efibootmgr` calls
- File: `src/mainwindow.cpp:520`
- File: `src/mainwindow.cpp:521`
- File: `src/mainwindow.cpp:522`
- File: `src/mainwindow.cpp:544`
- File: `src/mainwindow.cpp:547`
- File: `src/mainwindow.cpp:2011`
- Details:
  - `Cmd::procAsRoot()` passes arguments directly (no shell).
  - Several args still add literal quote characters (`"` / `'`) as if shell parsing existed.
- Impact:
  - Labels/loader/options can be written with unintended literal quotes or malformed command semantics.

### 3) Medium: Elevation command failure path is not robust
- File: `src/cmd.cpp:47`
- File: `src/cmd.cpp:95`
- File: `src/cmd.cpp:97`
- File: `src/cmd.cpp:92`
- Details:
  - Missing `pkexec/gksu` only logs warning.
  - Elevated exec still attempts `start(elevationCommand, ...)` even if command is empty.
  - Event loop waits on `done` (finished), with no `errorOccurred`/timeout escape path.
- Impact:
  - Can produce opaque failure behavior; potential wait/hang risk on failed process start.

### 4) Medium: Regex composition in crypttab parsing does not escape tokens
- File: `src/mainwindow.cpp:1260`
- Details:
  - Regex is built from `rootParentPatternList.join("|")` values derived from blkid/labels.
  - Tokens are not escaped before insertion into a regex.
- Impact:
  - Labels with regex meta chars can break matching or produce wrong matches.

### 5) Low: Helper hardening tests are not integrated into CTest
- File: `CMakeLists.txt:214`
- File: `CMakeLists.txt:227`
- File: `tests/test_helper.sh:1`
- Details:
  - `test_utils` is registered in CTest.
  - `test_helper.sh` is standalone and not run by `ctest`.
- Impact:
  - Helper parser/security regressions can slip through CI/test runs.

### 6) Low: Empty command string is currently accepted as success in helper
- File: `scripts/helper:125`
- File: `scripts/helper:142`
- File: `tests/test_helper.sh:141`
- File: `tests/test_helper.sh:143`
- Details:
  - `helper ""` passes validation and executes `bash -c ""` (exit 0).
- Impact:
  - Can mask upstream caller bugs where command construction accidentally produces empty strings.

---

## Prioritized TODO for Next LLM

1. ~~Fix `scripts/uefimanager-lib` dispatcher correctness and failure behavior.~~ DONE
   - Make command names consistent (`write_checkfile` everywhere).
   - Add `set -euo pipefail`.
   - Add default `case` branch returning non-zero for unknown subcommands.
   - Acceptance: direct invocation returns non-zero for invalid command; checkfile path works.

2. Remove literal shell-quote wrapping from all argv-style `efibootmgr` calls.
   - Update `installEfiStub()` and `renameUefiEntry()` argument assembly.
   - Keep Unicode/loader/options semantics intact without embedding quote characters.
   - Acceptance: resulting boot entries have expected labels/options (no extra quote chars).

3. Harden `Cmd::proc()` error path for elevation.
   - If elevation requested and no elevation command found: fail fast with user-visible error.
   - Connect `errorOccurred` to loop quit and map failure to actionable message.
   - Optionally add timeout safety for synchronous process execution.
   - Acceptance: no indefinite wait when elevated process fails to start.

4. Escape regex tokens in crypttab matching.
   - Escape each token in `rootParentPatternList` before joining with `|`.
   - Add/extend tests to include PARTLABEL values with regex metacharacters.
   - Acceptance: deterministic parsing for labels containing characters like `+`, `(`, `)`, `[` etc.

5. Integrate helper tests into CTest.
   - Add `add_test(NAME test_helper COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_helper.sh)` (or equivalent).
   - Gate on required binaries or provide fallback/mocks so CI behavior is deterministic.
   - Acceptance: `ctest` runs both unit tests and helper validation.

6. Decide and enforce policy for empty helper command strings.
   - Either reject empty/whitespace-only strings or document no-op behavior explicitly.
   - If rejecting, add tests and return non-zero with a clear error message.
   - Acceptance: behavior is intentional, documented, and test-covered.

---

## Suggested Execution Order
1. TODO #1 and #2 (highest user-visible correctness).
2. TODO #3 (robustness/safety).
3. TODO #4 (parsing correctness).
4. TODO #5 and #6 (test + policy hardening).
