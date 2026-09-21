# Shared by the shell-based suites: tiny PASS/FAIL reporting.
FAILS=0
pass() { echo "  PASS $1"; }
fail() { echo "  FAIL $1${2:+ ($2)}"; FAILS=$((FAILS+1)); }
check() { # check <desc> <command...>
  local d=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi
}
