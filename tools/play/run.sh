#!/bin/bash
# Scripted / live test runs of ACNH in an isolated data root (no game data here: the root links to private/acnh).
#
# usage: tools/play/run.sh <name> [options]      start a run in private/play/roots/<name>
#   --save <snapshot>   start from private/play/saves/<snapshot>/user (copied); default: a new user/ (the intro)
#   --keep-user         keep the root's user/ from its last run
#   --script <file>     NX_INPUT from gen.py <file>
#   --fast <frame>      NX_FAST_UNTIL=<frame>: 60 presents per second until that frame (the logic steps per frame)
#   --every-s <s>       NX_DUMP_EVERY_S=<s>: a frame every <s> seconds of wall time
#   --every <n>         NX_DUMP_FRAMES=<n>: a frame every <n> presents
#   --minutes <m>       stop the game after <m> minutes (default 8; 0: never)
#   --live              start in the background and return (drive it with live.py; stop it with `run.sh stop`)
#   --bin <path>        the game binary (default <data>/build/acnh)
#   --data <folder>     the install whose data and build the root links to (default private/acnh; a launcher or
#                       setup_acnh.py --out folder)
#   VAR=value           extra environment (NX_TIME_OFFSET=..., NX_TRACE_DRAWS=...)
# tools/play/run.sh stop <name>                    stop a live run
# tools/play/run.sh summary <name>                 fps, crashes, shader misses and hitches from the run's log
# tools/play/run.sh snapshot <name> <milestone> [--force]   copy the root's user/ to private/play/saves/<milestone>
# tools/play/run.sh pngs <name> [dir]              convert the dumped .ppm frames to 960-wide PNGs and delete them
#
# One game at a time per agent, in one of two run slots (private/run.slot1, run.slot2) shared by every agent: the Mac
# has 8 GB (a run takes ~2 GB). Frames are 6 MB each as .ppm: keep the dumps sparse.
# PLAY_SHOTS=<dir>: where live.py and `pngs` put PNGs (kept in the root as shots_dir).
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SRC=$REPO/private/acnh
PLAY=$REPO/private/play

summary() {
  local log=$1
  [ -f "$log" ] || { echo "no log: $log"; return 1; }
  echo "== $log"
  echo "-- fps (last lines)"
  grep -E 'fps' "$log" | grep -E 'nvn/vk: per frame|vk: frame' | tail -n 4
  echo "-- frames reached: $(grep -oE 'present [0-9]+' "$log" | tail -n 1 | grep -oE '[0-9]+')"
  echo "-- crashes: $(grep -c 'CRASH' "$log")"
  grep -A 12 'CRASH' "$log" | head -n 30
  echo "-- shader misses (no .spv): $(grep -cE 'no .*\.spv' "$log")"
  grep -E 'no .*\.spv' "$log" | head -n 5
  echo "-- hitches: $(grep -ciE 'hitch' "$log")"
  echo "-- last lines"
  tail -n 3 "$log"
}

case "$1" in
  stop)
    R=$PLAY/roots/$2
    if [ -f "$R/game.pid" ] && kill -0 "$(cat "$R/game.pid")" 2>/dev/null; then
      kill "$(cat "$R/game.pid")"
      for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$(cat "$R/game.pid")" 2>/dev/null || break; sleep 1; done
      kill -9 "$(cat "$R/game.pid")" 2>/dev/null
    fi
    rm -f "$R/game.pid"
    [ -f "$R/slot" ] && rmdir "$(cat "$R/slot")" 2>/dev/null; rm -f "$R/slot"
    summary "$R/nx.log"
    exit 0 ;;
  summary)
    summary "$PLAY/roots/$2/nx.log"; exit $? ;;
  snapshot)
    R=$PLAY/roots/$2 D=$PLAY/saves/$3
    [ -n "$3" ] || { echo "usage: run.sh snapshot <name> <milestone>"; exit 1; }
    [ -d "$R/user" ] || { echo "no user/ in $R"; exit 1; }
    if [ -d "$D" ] && [ "$4" != "--force" ]; then echo "$D exists (--force replaces it)"; exit 1; fi
    rm -rf "$D"; mkdir -p "$D"
    cp -R "$R/user" "$D/user"
    date "+%Y-%m-%d %H:%M:%S from $2" > "$D/when.txt"
    echo "saved $D ($(du -sh "$D" | cut -f1))"
    exit 0 ;;
  pngs)
    R=$PLAY/roots/$2
    OUT=${3:-$(cat "$R/shots_dir" 2>/dev/null || echo "$R/shots")}
    mkdir -p "$OUT"
    for p in "$R"/frames/*.ppm; do
      [ -f "$p" ] || continue
      sips -s format png -Z 960 "$p" --out "$OUT/$2_$(basename "$p" .ppm).png" >/dev/null 2>&1 && rm -f "$p"
    done
    ls "$OUT" | grep "^$2_" | tail -n 20
    exit 0 ;;
esac

NAME=$1
[ -n "$NAME" ] && [ "${NAME#-}" = "$NAME" ] || { sed -n '2,24p' "$0"; exit 1; }
shift
SAVE= KEEP= SCRIPT= FAST= EVERY_S= EVERY= MINUTES=8 LIVE= BIN=
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --save) SAVE=$2; shift ;;
    --keep-user) KEEP=1 ;;
    --script) SCRIPT=$2; shift ;;
    --fast) FAST=$2; shift ;;
    --every-s) EVERY_S=$2; shift ;;
    --every) EVERY=$2; shift ;;
    --minutes) MINUTES=$2; shift ;;
    --live) LIVE=1 ;;
    --bin) BIN=$2; shift ;;
    --data) SRC=$(cd "$2" && pwd) || exit 1; shift ;;
    *=*) EXTRA+=("$1") ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done
[ -n "$BIN" ] || BIN=$SRC/build/acnh

# One run of this tool at a time (the Mac has 8 GB); a game started elsewhere is only reported.
for pf in "$PLAY"/roots/*/game.pid; do
  [ -f "$pf" ] && kill -0 "$(cat "$pf")" 2>/dev/null && { echo "run: $(dirname "$pf") is still running"; exit 1; }
done
pgrep -fl '/acnh$' && echo "run: (another game process is running; memory: $(memory_pressure | tail -n 1))"
FREE_KB=$(df -k "$REPO" | awk 'NR==2 {print $4}')
if [ "$FREE_KB" -lt 3500000 ]; then echo "run: only $((FREE_KB / 1024)) MB free on disk"; exit 1; fi

R=$PLAY/roots/$NAME
mkdir -p "$R"
for d in analysis extract aoc fonts spirv exefs shaders_extracted mods; do
  [ -e "$SRC/$d" ] && [ ! -e "$R/$d" ] && ln -s "$SRC/$d" "$R/$d"
done
for f in "$SRC"/pipelines_*.bin "$SRC"/pipelines_*.keys "$SRC/users.txt" "$SRC/config.ini" "$SRC/language.txt"; do
  [ -f "$f" ] && [ ! -f "$R/$(basename "$f")" ] && cp "$f" "$R/"
done
rm -rf "$R/frames" "$R/input_live" "$R/dump_frames" "$R/live_frame" "$R/game.pid"
mkdir -p "$R/frames"
if [ -n "$SAVE" ]; then
  [ -d "$PLAY/saves/$SAVE/user" ] || { echo "no save $PLAY/saves/$SAVE/user"; exit 1; }
  rm -rf "$R/user"; cp -R "$PLAY/saves/$SAVE/user" "$R/user"
elif [ -z "$KEEP" ]; then
  rm -rf "$R/user"
fi
[ -n "$PLAY_SHOTS" ] && echo "$PLAY_SHOTS" > "$R/shots_dir"
[ -f "$R/nx.log" ] && mv "$R/nx.log" "$R/nx.prev.log"

export NX_DATA_ROOT=$R NX_LOG=$R/nx.log NX_HEADLESS=1 NX_LIVE_FRAME=1 SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER:-dummy}
if [ -n "$SCRIPT" ]; then
  NX_INPUT=$(python3 "$REPO/tools/play/gen.py" "$SCRIPT") || exit 1
  export NX_INPUT
  python3 "$REPO/tools/play/gen.py" "$SCRIPT" 2>&1 >/dev/null | tail -n 20
fi
[ -n "$FAST" ] && export NX_FAST_UNTIL=$FAST
[ -n "$EVERY_S" ] && export NX_DUMP_EVERY_S=$EVERY_S
[ -n "$EVERY" ] && export NX_DUMP_FRAMES=$EVERY
for kv in "${EXTRA[@]}"; do export "${kv?}"; done

# A run slot (shared by every agent on this Mac: two games at most), released when the game exits.
until mkdir "$REPO/private/run.slot1" 2>/dev/null && SLOT=$REPO/private/run.slot1 ||
      { mkdir "$REPO/private/run.slot2" 2>/dev/null && SLOT=$REPO/private/run.slot2; }; do
  echo "run: waiting for a run slot"; sleep 10
done
echo "$SLOT" > "$R/slot"
"$BIN" > "$R/stdout.txt" 2>&1 &
PID=$!
echo $PID > "$R/game.pid"
echo "run: $NAME pid $PID, root $R, $(basename "$SLOT")"
if [ -n "$LIVE" ]; then
  # Detached: release the slot when the game exits, and stop a forgotten game (it holds 2 GB) after --minutes.
  nohup bash -c "S=\$(date +%s); while kill -0 $PID 2>/dev/null; do sleep 5;
    if [ $MINUTES != 0 ] && [ \$(( \$(date +%s) - S )) -ge $((MINUTES * 60)) ]; then kill $PID; fi
    [ \$(df -k '$REPO' | awk 'NR==2 {print \$4}') -lt 3100000 ] && kill $PID; done;
    rmdir '$SLOT' 2>/dev/null; rm -f '$R/slot' '$R/game.pid'" >/dev/null 2>&1 &
  exit 0
fi
trap 'kill $PID 2>/dev/null; rmdir "$SLOT" 2>/dev/null; rm -f "$R/slot"' EXIT
START=$(date +%s)
while kill -0 $PID 2>/dev/null; do
  sleep 5
  if [ "$MINUTES" != 0 ] && [ $(( $(date +%s) - START )) -ge $((MINUTES * 60)) ]; then
    kill $PID; sleep 5; kill -9 $PID 2>/dev/null
  fi
  FREE_KB=$(df -k "$REPO" | awk 'NR==2 {print $4}')
  if [ "$FREE_KB" -lt 3100000 ]; then echo "run: disk below 3 GB, stopping"; kill $PID; fi
done
wait $PID 2>/dev/null
echo "run: exit status $?"
rm -f "$R/game.pid"
summary "$R/nx.log"
echo "-- frames dumped: $(ls "$R/frames" | wc -l | tr -d ' ') (run.sh pngs $NAME)"
