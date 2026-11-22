
# keylog-bof

Windows keylogger for AdaptixC2, packaged as a Beacon Object File (BOF). Polls the keyboard through `GetAsyncKeyState.

No hooks. No `BeaconDataParse`. No CRT imports. Dynamic resolution: kernel32 is found via a PEB, everything else via `GetProcAddress`.

## What it does

- Captures keystrokes globally, focused application.
- Prints the foreground window title on change.
- Detects native `Edit` controls with `ES_PASSWORD` style.
- Runs for 60 seconds by default (adjustable in `keylog.c`).
- Streams output through the beacon channel, flushes on 4 KB or end of run.

## Output format

- Window title appears once per foreground-window change, wrapped in `[ ]`.
- Special keys are written as `<BS>`, `<ESC>`, `<DEL>`, `<TAB>`, `<L>`, `<R>`, `<U>`, `<D>`, `<HOME>`, `<END>`, `<PGUP>`, `<PGDN>`.
- Password typing is wrapped between `[PASSWORD]` and `[END PASSWORD]` markers.

## Requirements

- AdaptixC2 client with the Extension-Kit loaded.
- mingw-w64 (x86_64) for compilation.
- A Kharon / beacon / gopher agent on a Windows x64 target.

## Build

### Prerequisites

```bash
# Debian / Kali
apt install gcc-mingw-w64-x86-64-posix mingw-w64-tools

# Arch
pacman -S mingw-w64-x86_64-gcc

### Compile the BOF

x86_64-w64-mingw32-gcc -c keylog.c -o keylog.o -O0 -Wall -D_WIN32_WINNT=0x0601
```

The `-O0` is intentional. At `-O2`, GCC emits `memcpy` calls for loop patterns, which the AdaptixC2 BOF loader does not resolve. At `-O0`, no builtin CRT symbols are emitted.

### Verify imports

```bash
x86_64-w64-mingw32-nm keylog.o | grep ' U '
```

The only unresolved symbol is `BeaconOutput`, which the AdaptixC2 loader provides. Everything else is resolved at runtime from the PEB walk and `GetProcAddress`.

### Place the object

```bash
mkdir -p _bin
cp keylog.o _bin/keylog.x64.o
```

The `_bin/` directory is where the `.axs` script expects the object at load time.

## Deploy

1. Copy `keylog.c`, `keylog.axs`, and `_bin/keylog.x64.o` into your AdaptixC2 client's `Extension-Kit/KeyLog-BOF/` directory.
2. Add one line to `Extension-Kit/extension-kit.axs`, near the other `ax.script_load` calls:

   ```javascript
   ax.script_load(path + "KeyLog-BOF/keylog.axs");
   ```

3. Load the top-level loader in the client:

   **Main menu → AxScript → Script manager → Load new → `extension-kit.axs`**

4. On a beacon session, right-click → `keylog` (or type `keylog` in the session console).

## Output delivery

`BeaconOutput` is called from the BOF's own thread (the beacon's job thread). No worker thread, no hook callback, no cross-thread output — the beacon stays stable under repeated use.

Output flushes on buffer full, end of capture window, and window / password marker transitions.

## Limitations

- **Web password fields** (Gmail, Facebook, HTML `<input type=password>`) are not native `Edit` controls. `GetClassNameA` returns a browser widget class, so `[PASSWORD]` markers are not emitted. Keystrokes are still captured, without the marker.
- **Elevated windows** — a beacon running at Medium integrity does not observe keystrokes typed into High-integrity windows. For that, the beacon would need to run elevated.
- **Lock screen** — `GetAsyncKeyState` returns nothing while the secure desktop is active.
- **Unicode non-ASCII** — only `0x20-0x7F` is captured. Accented characters and CJK scripts are dropped.
- **Fast typing** — 25 ms polling can miss keys pressed faster than that. For normal typing it is invisible.
- **Localized keyboard layouts** — `ToUnicode` is called with scancode `0` (since `GetAsyncKeyState` gives only the virtual key). On non-US layouts, shifted characters may come out wrong.

## Architecture

- PEB walk via inline asm to locate `kernel32.dll`.
- Export parsing to locate `GetProcAddress`.
- `LoadLibraryA` for `user32.dll`.
- Runtime resolution of `Sleep`, `GetForegroundWindow`, `GetWindowTextA`, `GetKeyboardState`, `ToUnicode`, `GetAsyncKeyState`, `GetGUIThreadInfo`, `GetClassNameA`, `GetWindowLongA`, `GetWindowThreadProcessId`.
- `poll_keys()` runs every 25 ms in `go()`'s loop, calling `capture_context()` for title / password markers and `emit_vk()` for character translation.
- No `SetWindowsHookEx`, no `CreateThread`. All work happens in `go()`'s loop. When `go()` returns, the BOF is gone and nothing survives.

## Why polling instead of a hook

`WH_KEYBOARD_LL` via `SetWindowsHookExA` requires the process hosting the hook to stay mapped. When the BOF returns, the AdaptixC2 loader frees the BOF's memory; if the hook is still registered, the next keystroke calls into freed memory and crashes the agent. Polling with `GetAsyncKeyState` avoids this entirely: no callback, no freed code, no crash.

The trade-off is 25 ms of latency and occasional missed keystrokes during very fast typing.

## Build flags reference

| Flag | Why |
|---|---|
| `-O0` | No `memcpy` / `memset` / `strcmp` builtins emitted |
| `-D_WIN32_WINNT=0x0601` | Windows 7+ API level (`GUITHREADINFO`, `ES_PASSWORD`) |
| `-fno-builtin` | Optional, only if `-O0` ever needs to be replaced with `-O2` |

## License

MIT — see `LICENSE`.
```
