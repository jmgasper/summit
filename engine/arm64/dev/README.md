# arm64 bring-up helpers

Small tools from bringing up the arm64 engine on the ROCK 5. They lived only
in the build directory (`build/summit-arm64`).

| File | What it is |
|---|---|
| `crash.c` | an `LD_PRELOAD` crash reporter (`libsummitcrash.so`): prints PC, LR, SP and a backtrace when Summit or an engine process faults |
| `relaunch.sh` | stops a running arm64 Summit and starts it again with the crash reporter preloaded |
| `nbtest/nb*.c` | non-blocking socket probes, written while finding curl's `SOCK_NONBLOCK` problem on Haiku |
