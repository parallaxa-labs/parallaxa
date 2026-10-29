<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Stage control: the same UI in AI Live Studio's Stage dock and page and in the standalone
  dock (dock-standalone.html). Works at 300 px: one column of cards; in wider
  layouts the cards flow into two columns. Sections: Looks, Camera (views pad, auto camera,
  intensity, freeze, home, moves), Manual camera, Layers, Depth of field, Effects.
  Live-safe bar: on-air lock (hold Alt or "Unlock for 10 s"), Undo (last 20 slider changes),
  Panic reset (vendor `panic`, always enabled; a host with its own Panic button can relabel it,
  e.g. "Reset camera & effects"). While Stage is not reachable, a banner says why, with
  "Check again" (and any extra actions the host passes in `fix`). Screen keys: 1-9 views, H home, F freeze,
  Ctrl+Z undo. The Manual camera card and the Layers / Depth of field cards come from the
  `manual` and `layers` loaders when they are first open: collapsed in docks, so a host that
  code-splits loads that code only on demand; wide layouts open them, and load them, at once. The standalone dock passes its static imports (one inline
  file, no dynamic import).
-->
<script lang="ts">
  import { untrack, type Component, type Snippet } from 'svelte';
  import { AUTO_MODES, type AutoMode } from '../lib/protocol/vendor.ts';
  import { isTextTarget } from '../lib/ui/text-target.ts';
  import type { StageHost } from './host.ts';
  import Effects from './Effects.svelte';
  import Looks from './Looks.svelte';
  import { StageModel } from './model.svelte.ts';
  import Range from './Range.svelte';
  import './stage.css';
  import { t } from './t.ts';
  import ViewPad from './ViewPad.svelte';

  interface Props {
    host: StageHost;
    layout?: 'wide' | 'compact' | 'dock';
    /** Extra cards after Effects (host-specific). */
    extra?: Snippet<[StageModel]>;
    /** Extra content at the end (standalone: connection settings). */
    footer?: Snippet;
    /** Extra actions in the connection banner (e.g. a link to the host's settings). */
    fix?: Snippet;
    /** Label of the Stage reset button, for a host that has its own Panic. */
    panicLabel?: string;
    /** Loads the Manual camera card (a dynamic import, or in the standalone dock its static import). */
    manual: () => Promise<{ default: Component<{ m: StageModel }> }>;
    /** Loads the Layers and Depth of field cards' body (Layers.svelte), the same way. */
    layers: () => Promise<{ default: Component<{ m: StageModel; part: 'layers' | 'dof' }> }>;
  }
  let { host, layout, extra, footer, fix, panicLabel, manual: loadManualCard, layers: loadLayersCard }: Props = $props();
  const m = new StageModel(host);
  // No destructuring default (`layout = 'dock'`): it would pull Svelte's prop() helper into the standalone dock.
  const dock = $derived(!layout || layout === 'dock');
  const GET_STAGE = 'https://github.com/parallaxa-labs/parallaxa';

  // Loaded by the card's toggle event, which also fires when it starts open (wide layouts).
  let Manual = $state.raw<Component<{ m: StageModel }> | null>(null);
  const loadManual = () => Manual || void loadManualCard().then((mod) => (Manual = mod.default));
  let Layers = $state.raw<Component<{ m: StageModel; part: 'layers' | 'dof' }> | null>(null);
  const loadLayers = () => Layers || void loadLayersCard().then((mod) => (Layers = mod.default));

  $effect(() => m.listen());
  // Load when Stage becomes reachable and after every reconnection. `ready` is memoized:
  // a host may update host.conn every second.
  const ready = $derived(host.conn === 'ready');
  $effect(() => {
    if (!ready) return;
    void host.epoch;
    untrack(() => void m.load());
  });
  // A new target scene reloads the scene's state, layers and views.
  let lastScene = untrack(() => host.scene);
  $effect(() => {
    const s = host.scene;
    if (s === lastScene) return;
    lastScene = s;
    if (untrack(() => ready)) void m.loadScene();
  });

  function keydown(e: KeyboardEvent) {
    if (e.key === 'Alt') m.alt = true;
    if (e.defaultPrevented || e.repeat || e.metaKey || isTextTarget(e.target) || host.conn !== 'ready') return;
    const k = e.key.toLowerCase();
    if (e.ctrlKey) {
      if (k === 'z' && !e.shiftKey && !e.altKey) {
        e.preventDefault();
        void m.undo();
      }
      return;
    }
    if (e.altKey) return;
    if (/^[1-9]$/.test(k)) m.applyView(Number(k));
    else if (k === 'h') m.home();
    else if (k === 'f') void m.freeze();
    else return;
    e.preventDefault();
  }
  const altUp = (e: KeyboardEvent) => e.key === 'Alt' && (m.alt = false);
  // Alt held while the OBS main window has the keyboard: the dock sees no key events, but
  // pointer events carry the modifier, so hovering the dock with Alt held unlocks it before
  // the press (a disabled slider cannot start a drag). Leaving the dock ends it.
  const altPtr = (e: PointerEvent) => m.alt !== e.altKey && (m.alt = e.altKey);

  const st = $derived(m.st);
  const mode = $derived<AutoMode>(st?.auto.mode ?? 'off');
  /** Auto camera radio group: arrow keys move and select. */
  function segKey(e: KeyboardEvent, i: number) {
    const d = ({ ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 } as Record<string, number>)[e.key];
    if (!d) return;
    e.preventDefault();
    const n = (i + d + AUTO_MODES.length) % AUTO_MODES.length;
    m.auto(AUTO_MODES[n] as AutoMode);
    ((e.currentTarget as HTMLElement).parentElement?.children[n] as HTMLElement | undefined)?.focus();
  }
  const rigs = $derived(m.info?.rigs.filter((r) => r.enabled).map((r) => r.scene) ?? []);
  const banner = $derived(
    ({ connecting: 'stage.looking', offline: 'stage.offline', obs_down: 'stage.obs_down', ws_off: 'stage.obs_closed', auth: 'stage.obs_auth', no_stage: 'stage.not_found' } as Record<string, string>)[
      host.conn
    ],
  );
</script>

<svelte:window
  onkeydown={keydown}
  onkeyup={altUp}
  onblur={() => (m.alt = false)}
  onpointermove={altPtr}
  onpointerdown={altPtr}
  onpointerout={(e) => !e.relatedTarget && (m.alt = false)}
/>

{#snippet card(title: string, open: boolean, body: Snippet, opened: (() => void) | null = null)}
  <details class="card" {open} ontoggle={opened && ((e) => e.currentTarget.open && opened())}>
    <summary><h2>{title}</h2></summary>
    <div class="body">{@render body()}</div>
  </details>
{/snippet}

<div class="stage {dock ? '' : 'wide'}">
  {#if banner}
    <section class="card state" role="status">
      <p>{t(banner)}</p>
      {#if host.conn === 'no_stage'}
        <p class="note">{t('stage.restart_obs')}</p>
        <div class="row">
          <a class="b p" href={GET_STAGE} target="_blank" rel="noopener noreferrer">{t('stage.get')}</a>
          <button type="button" class="b" onclick={() => host.retry()}>{t('stage.check_again')}</button>
        </div>
      {:else if host.conn !== 'connecting' && host.conn !== 'offline'}
        <div class="row">
          <button type="button" class="b" onclick={() => host.retry()}>{t('stage.retry')}</button>
          {@render fix?.()}
        </div>
      {/if}
    </section>
  {:else}
    <div class="top">
      <label class="scene">
        <span>{t('stage.target_scene')}</span>
        <!-- {@attach}, not value=: Svelte's select runtime would add 0.4 KB gz to the standalone dock. -->
        <select {@attach (el) => void (el.value = host.scene)} onchange={(e) => host.setScene(e.currentTarget.value)}>
          <option value="">{t('stage.program_scene')}</option>
          {#each host.scene && !rigs.includes(host.scene) ? [...rigs, host.scene] : rigs as s (s)}<option value={s}>{s}</option>{/each}
        </select>
      </label>
      <button type="button" class="b g" title={t('stage.reload')} aria-label={t('stage.reload')} onclick={() => m.load()}>↻</button>
    </div>
    {#if st && m.info}<p class="note">{t('stage.detected', { version: m.info.version, scene: st.scene })}</p>{/if}
    {#if m.noRig}<p class="warn" role="alert">{t('stage.no_rig')}</p>{/if}

    <div class="bar" role="group" aria-label={t('stage.livesafe_bar')}>
      {#if host.streaming}
        <span class="lock {m.locked ? '' : 'open'}">{m.locked ? t('stage.locked_live') : m.unlockLeft > 0 ? t('stage.unlocked', { s: m.unlockLeft }) : t('stage.unlocked_alt')}</span>
        <button type="button" class="b" disabled={m.unlockLeft > 0} onclick={() => m.unlock()}>{t('stage.unlock_10s')}</button>
      {/if}
      <button type="button" class="b" disabled={!m.undos.length || m.locked} title={m.undos.at(-1)?.label} onclick={() => m.undo()}>
        {t('stage.undo')}{m.undos.length ? ` (${m.undos.length})` : ''}
      </button>
      <button type="button" class="b d lg panic" onclick={() => m.panic()}>{panicLabel ?? t('stage.panic')}</button>
    </div>
    {#if host.streaming && m.locked}<p class="note">{t('stage.locked_hint')}</p>{/if}
    <p class="notice {m.notice?.error ? 'err' : ''}" aria-live="polite">{m.notice?.text ?? ''}</p>

    <div class="cards">
      {#snippet looks()}<Looks {m} />{/snippet}
      {@render card(t('stage.looks'), true, looks)}

      {#snippet camera()}
        <ViewPad {m} />
        <div class="row tools">
          <button type="button" class="b" onclick={() => m.home()}>{t('stage.home')} <kbd>H</kbd></button>
          <button type="button" class="b" aria-pressed={!!st?.frozen} onclick={() => m.freeze()}>{t(st?.frozen ? 'stage.frozen' : 'stage.freeze')} <kbd>F</kbd></button>
        </div>
        <div class="seg" role="radiogroup" aria-label={t('stage.auto_camera')}>
          {#each AUTO_MODES as v, i (v)}
            <!-- Docks are narrow: one short word per mode, the full name as the tooltip and accessible name. -->
            <button type="button" role="radio" aria-checked={v === mode} tabindex={v === mode ? 0 : -1} title={dock ? t(`stage.auto.${v}`) : undefined} aria-label={dock ? t(`stage.auto.${v}`) : undefined} onclick={() => m.auto(v)} onkeydown={(e) => segKey(e, i)}>{t(dock ? `stage.auto_short.${v}` : `stage.auto.${v}`)}</button>
          {/each}
        </div>
        <Range
          label={t('stage.intensity')}
          unit="%"
          value={st ? Math.round(st.auto.intensity * 100) : undefined}
          min={0}
          max={100}
          def={35}
          locked={m.locked} onair={m.host.streaming}
          oninput={(v) => m.auto(mode, v / 100)}
          onchange={(p) => m.remember(t('stage.intensity'), () => m.auto(mode, p / 100))}
        />
        {#if m.moves.length}
          <h3>{t('stage.moves')}</h3>
          <div class="row moves">
            {#each m.moves as mv (mv.id)}
              <button type="button" class="b" onclick={() => m.move(mv.id)}>
                {mv.name[host.lang] || mv.name.en || mv.id}{#if !mv.builtin}<span class="badge">{t('stage.badge_user')}</span>{/if}
              </button>
            {/each}
          </div>
        {/if}
      {/snippet}
      {@render card(t('stage.camera'), true, camera)}

      {#snippet manual()}{#if Manual}<Manual {m} />{/if}{/snippet}
      {@render card(t('stage.manual'), !dock, manual, loadManual)}

      {#snippet layers()}{#if Layers}<Layers {m} part="layers" />{/if}{/snippet}
      {@render card(t('stage.layers'), !dock, layers, loadLayers)}

      {#snippet dof()}{#if Layers}<Layers {m} part="dof" />{/if}{/snippet}
      {@render card(t('stage.dof'), !dock, dof, loadLayers)}

      {#snippet effects()}<Effects {m} {dock} />{/snippet}
      {@render card(t('stage.effects'), true, effects)}

      {#if extra}{@render extra(m)}{/if}
    </div>
    <div class="row foot">
      <button type="button" class="b g" role="switch" aria-checked={host.reduceMotion} onclick={() => host.setReduceMotion(!host.reduceMotion)}>
        {t('stage.reduce_motion')}: {t(host.reduceMotion ? 'stage.on' : 'stage.off')}
      </button>
    </div>
  {/if}
  {#if footer}{@render footer()}{/if}
</div>

<style>
  @layer screens {
    .stage { display: flex; flex-direction: column; gap: var(--sp-4); min-width: 0; }
    .cards { display: grid; gap: var(--sp-4); align-items: start; }
    @container (min-width: 720px) {
      .wide .cards { grid-template-columns: 1fr 1fr; }
    }
    .body { display: flex; flex-direction: column; gap: var(--sp-5); padding: 0 var(--sp-5) var(--sp-5); }
    .state { display: flex; flex-direction: column; gap: var(--sp-4); padding: var(--sp-6); }
    .top { display: flex; align-items: flex-end; gap: var(--sp-3); }
    .scene { flex: 1 1 auto; }
    .warn { padding: var(--sp-4); border: 1px solid var(--c-warning); border-radius: var(--r-md); color: var(--c-warning); font-size: var(--fs-12); }
    .bar {
      position: sticky;
      top: 0;
      z-index: var(--z-sticky);
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--sp-3);
      padding: var(--sp-3) 0;
      background: var(--c-bg);
    }
    .lock { flex: 1 1 100%; font-weight: var(--fw-bold); color: var(--c-live); font-size: var(--fs-12); }
    .lock::before { content: '● '; }
    .lock.open { color: var(--c-warning); }
    .panic { margin-inline-start: auto; }
    .notice { min-height: 1.2em; margin: 0; font-size: var(--fs-12); color: var(--c-text-muted); }
    .notice.err { color: var(--c-danger); }
    h3 { font-size: var(--fs-12); color: var(--c-text-muted); margin: 0; }
    .seg { display: grid; grid-auto-flow: column; grid-auto-columns: 1fr; gap: 2px; padding: 2px; border: 1px solid var(--c-border-strong); border-radius: var(--r-md); background: var(--c-surface-2); }
    .seg button { min-height: var(--hit-min); padding: var(--sp-2); border: 0; border-radius: var(--r-sm); background: none; color: var(--c-text-muted); font-size: var(--fs-12); font-weight: var(--fw-medium); line-height: var(--lh-tight); }
    .seg button:hover { background: var(--c-surface-3); color: var(--c-text); }
    .seg [aria-checked='true'], .seg [aria-checked='true']:hover { background: var(--c-primary); color: var(--c-on-primary); }
    .moves .b { padding: 0 var(--sp-4); font-size: var(--fs-12); }
    .tools .b { flex: 1 1 0; }
    kbd { font-size: var(--fs-11); opacity: .7; }
    .foot { justify-content: flex-end; }
  }
</style>
