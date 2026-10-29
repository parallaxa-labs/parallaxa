# Standalone Stage dock: source

`../data/dock/dock-standalone.html` is a minified, self-contained build of the
Svelte 5 sources in this folder: the Parallaxa control dock that OBS opens as a
`file://` Custom Browser Dock, without AI Live Studio. It talks to OBS over
obs-websocket (vendor `parallaxa`, `../docs/protocol/vendor-api.md`).

These files are part of Parallaxa and are licensed under the GNU General
Public License, version 2 or (at your option) any later version
(`../LICENSE`). Parallaxa Labs, the copyright holder, also uses some of them in
its AI Live Studio product under other terms; that does not limit your rights
under the GPL for the files published here.

## Build

Node.js 22.12+ and pnpm 11 (https://pnpm.io):

```powershell
cd dock-src
pnpm install --frozen-lockfile
pnpm build          # vite build --mode standalone -> ../data/dock/dock-standalone.html
pnpm verify         # svelte-check, vitest, and "a fresh build equals the shipped file"
pnpm gen-dict       # after editing src/**/i18n/*.json: regenerate src/dock/standalone-dict.gen.ts
```

Commit the rebuilt `data/dock/dock-standalone.html` together with the source
change; CI runs `pnpm verify`, which fails when the two differ. The JS budget
of the dock is 32 KB gzipped (`src/dock/standalone.test.ts`).

Keep the clone path short (for example `C:\src\parallaxa`). pnpm nests
packages deep inside `node_modules\.pnpm`, and when a path there passes 260
characters Node.js cannot read the package's `imports` map, even with Windows
long paths enabled. vitest then fails with `ERR_PACKAGE_IMPORT_NOT_DEFINED`
(`"#module-evaluator" is not defined`). Move the checkout to a shorter path
and run `pnpm install` again.

## Layout

The folder keeps the layout of the web project it comes from (`src/dock/`,
`src/lib/`, `src/styles/`, `scripts/vite-plugins.ts`), reduced to the files
the standalone build uses. The string catalogues (`src/lib/i18n/*.json`,
`src/dock/i18n/*.json`) hold only the keys the standalone dock shows;
`src/dock/standalone.test.ts` fails when they hold any other key.
`package.json`, `vite.config.ts`, the tests and `scripts/verify-dock.mjs`
belong to this repository; the other files are kept in step with AI Live
Studio's copy by Parallaxa Labs (see CONTRIBUTING.md, "Dock sources").
Outside contributions to this folder need a signed contributor licence
agreement (`../CLA.md`). `src/lib/protocol/vendor.ts` is the TypeScript
mirror of the vendor protocol in `../docs/protocol`.
