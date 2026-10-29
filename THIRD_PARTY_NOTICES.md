# Third-party notices

Parallaxa (`parallaxa.dll` and its `data\` folder) is free software under the
GNU General Public License, version 2 or (at your option) any later version
(`LICENSE`). It contains or is built from the following third-party code.
The release zip and the installer ship this file and the `licenses\` folder.

| Component | Version | Licence | Shipped as | Licence text |
| --- | --- | --- | --- | --- |
| [OBS Studio](https://github.com/obsproject/obs-studio) (libobs and obs-frontend-api headers) | 32.0.0 | GPL-2.0-or-later | compiled-in header code; links `obs.dll` at run time | `LICENSE` |
| obs-websocket plugin API header (`third_party/obs-websocket-api.h`) | API version 3 | GPL-2.0-or-later | compiled-in header code | `LICENSE` |
| [pthreads-win32](https://sourceware.org/pthreads-win32/) (`w32-pthreads`, as in OBS Studio's `deps/w32-pthreads`), Copyright (C) 1998 John E. Bossom, (C) 1999, 2005 Pthreads-win32 contributors | 2.9.1 | LGPL-2.1-or-later | compiled-in header code (`pthread.h`, `sched.h`); links `w32-pthreads.dll`, which OBS Studio ships, at run time (not redistributed) | [licenses/w32-pthreads/COPYING.LIB.txt](licenses/w32-pthreads/COPYING.LIB.txt) (LGPL 2.1), [licenses/w32-pthreads/COPYING.txt](licenses/w32-pthreads/COPYING.txt) (copyright notice) |
| [svelte](https://www.npmjs.com/package/svelte) | 5.35.7 | MIT | inside `data\dock\dock-standalone.html` | [licenses/svelte/LICENSE.md](licenses/svelte/LICENSE.md) |
| [esm-env](https://www.npmjs.com/package/esm-env) | 1.2.2 | MIT | used at build time by svelte (resolved to constants, so none of its code ends up in `data\dock\dock-standalone.html`) | [licenses/esm-env/LICENSE.txt](licenses/esm-env/LICENSE.txt) |

The npm packages are the ones the standalone dock build uses (`dock-src/`,
locked by `dock-src/pnpm-lock.yaml`). Update this table when a dock dependency
changes. Build-time tools (Vite, TypeScript, terser, svelte-check, vitest,
MSVC, CMake, Ninja, Inno Setup) are not distributed and are not listed.

The presets, LUTs, sample images, effect shaders and locale files in `data\`
are original works of Parallaxa Labs under the plugin's licence.
