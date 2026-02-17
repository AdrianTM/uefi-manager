#!/bin/bash
#
# Unit tests for scripts/helper command validation.
#

set -euo pipefail

HELPER="$(dirname "$0")/../scripts/helper"
PASS=0
FAIL=0

# Run a test case.
#   expect_ok  <description> <args...>  — expect exit 0
#   expect_err <description> <args...>  — expect non-zero exit
expect_ok() {
    local desc="$1"; shift
    if "$HELPER" "$@" >/dev/null 2>&1; then
        ((++PASS))
    else
        echo "FAIL (expected ok): $desc" >&2
        ((++FAIL))
    fi
}

expect_err() {
    local desc="$1"; shift
    if "$HELPER" "$@" >/dev/null 2>&1; then
        echo "FAIL (expected error): $desc" >&2
        ((++FAIL))
    else
        ((++PASS))
    fi
}

# Check that a specific error message appears on stderr.
#   expect_err_msg <description> <pattern> <args...>
expect_err_msg() {
    local desc="$1"; shift
    local pattern="$1"; shift
    local stderr
    stderr="$("$HELPER" "$@" 2>&1 >/dev/null || true)"
    if [[ "$stderr" == *"$pattern"* ]]; then
        ((++PASS))
    else
        echo "FAIL (expected '$pattern' in stderr): $desc — got: $stderr" >&2
        ((++FAIL))
    fi
}

echo "=== Fast path (multi-arg) tests ==="

expect_ok   "allowed command: lsblk"          lsblk --version
expect_ok   "allowed command: grep"           grep --version
expect_ok   "allowed command: efibootmgr"     efibootmgr --version
expect_err  "disallowed command: bash"         bash -c "echo hi"
expect_err  "disallowed command: sh"           sh -c "echo hi"
expect_err  "disallowed command: cat"          cat /etc/passwd
expect_err  "disallowed command: python"       python3 -c "print(1)"
expect_err  "disallowed command: chmod"        chmod 777 /tmp
expect_err  "disallowed command: chown"        chown root /tmp

echo "=== No arguments ==="

expect_err_msg "no args" "no command provided"

echo "=== Allowed full-path command ==="

# This path won't exist in test env, so it will fail to exec, but it should
# pass validation (exit code from missing binary, not from permission check).
# We just verify it doesn't say "not permitted".
stderr="$("$HELPER" /usr/lib/uefi-manager/uefimanager-lib 2>&1 || true)"
if [[ "$stderr" == *"not permitted"* ]]; then
    echo "FAIL: full-path command rejected" >&2
    ((++FAIL))
else
    ((++PASS))
fi

echo "=== Shell construct blocking ==="

expect_err_msg "command substitution \$()"   "forbidden shell constructs"  'echo $(whoami)'
expect_err_msg "backtick substitution"       "forbidden shell constructs"  'echo `whoami`'
expect_err_msg "process substitution <()"    "forbidden shell constructs"  'grep foo <(cat /etc/passwd)'
expect_err_msg "process substitution >()"    "forbidden shell constructs"  'grep foo >(cat /etc/passwd)'

echo "=== I/O redirection blocking ==="

expect_err_msg "output redirect >"       "redirections are not permitted"  'grep foo /dev/null > /tmp/out'
expect_err_msg "append redirect >>"      "redirections are not permitted"  'grep foo /dev/null >> /tmp/out'
expect_err_msg "input redirect <"        "redirections are not permitted"  'grep foo < /etc/passwd'

echo "=== Redirects inside quotes are allowed ==="

# grep ">" in /dev/null — the > is inside quotes, should pass validation
# (grep itself returns 1 because no match, but that's fine)
stderr="$("$HELPER" 'grep ">" /dev/null' 2>&1 || true)"
if [[ "$stderr" == *"redirections"* ]]; then
    echo "FAIL: redirect inside quotes was blocked" >&2
    ((++FAIL))
else
    ((++PASS))
fi

echo "=== Pipe chain validation ==="

expect_ok      "pipe with allowed commands"       'grep --version | cut -d" " -f1'
expect_err_msg "pipe with disallowed command"  "not permitted"  'grep foo | bash -c "evil"'
expect_err_msg "pipe with cat"                 "not permitted"  'lsblk | cat'

echo "=== && chain validation ==="

expect_ok      "&& with allowed commands"          'grep --version && lsblk --version'
expect_err_msg "&& with disallowed command"    "not permitted"  'grep --version && bash -c "evil"'

echo "=== || chain validation ==="

expect_ok      "|| with allowed commands"          'grep --version || lsblk --version'
expect_err_msg "|| with disallowed command"    "not permitted"  'grep --version || bash evil'

echo "=== ; chain validation ==="

expect_ok      "; with allowed commands"           'grep --version ; lsblk --version'
expect_err_msg "; with disallowed command"     "not permitted"  'grep --version ; bash -c evil'

echo "=== Bare & validation ==="

expect_err_msg "bare & with disallowed cmd"    "not permitted"  'grep --version & bash -c evil'

echo "=== Quoted command names ==="

expect_ok   "single-quoted command name"       "'grep' --version"
expect_ok   "double-quoted command name"       '"grep" --version'

echo "=== Malformed input ==="

expect_err_msg "unterminated single quote"  "malformed"  "grep 'foo"
expect_err_msg "unterminated double quote"  "malformed"  'grep "foo'

echo "=== Empty/whitespace input ==="

# Empty/whitespace command strings are no-ops (bash -c "" exits 0), which is safe
expect_ok "empty string is harmless no-op"       ""
expect_ok "whitespace only is harmless no-op"    "   "

echo ""
echo "Results: $PASS passed, $FAIL failed"
if ((FAIL > 0)); then
    exit 1
fi
