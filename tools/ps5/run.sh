#!/usr/bin/env bash
# One entry point for the log-driven PS5 feasibility pipeline.
#   ./tools/ps5/run.sh                                  # all stages
#   ./tools/ps5/run.sh --stages env,inspect,extract,probe --games oot
#   ./tools/ps5/run.sh --stages compile --max-compile-files 0
# Override locations with SWR_WORK / SWR_RUN_ID.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=${SWR_WORK:-/data/switchrecomp}
RUN=${SWR_RUN_ID:-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$WORK/runs/$RUN"
echo "repo=$ROOT"
echo "run=$WORK/runs/$RUN"
python3 "$ROOT/tools/ps5/pipeline.py" --work-root "$WORK" --run-id "$RUN" "$@"
rc=$?
echo "RUN_DIR=$WORK/runs/$RUN"
echo "EXIT=$rc"
exit "$rc"
