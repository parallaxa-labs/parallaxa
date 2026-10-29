<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Looks: a horizontal row of recipe cards from recipe_list, each with a CSS preview
  (animated only on hover / focus), the localized name, tags, Pro / Yours badges and Apply
  (recipe_apply). While live, Apply asks first in a small inline confirm (not a modal); the
  confirm can be turned off per Look (kept in this browser).
-->
<script lang="ts">
  import type { RecipeInfo } from '../lib/protocol/vendor.ts';
  import type { StageModel } from './model.svelte.ts';
  import { t } from './t.ts';

  let { m }: { m: StageModel } = $props();
  const KEY = 'parallaxa.noask';
  let noAsk = $state.raw<string[]>(read());
  let asking = $state.raw('');
  let skip = $state.raw(false);

  function read(): string[] {
    try {
      const v = JSON.parse(localStorage.getItem(KEY) ?? '[]') as unknown;
      return Array.isArray(v) ? v.filter((x) => typeof x === 'string') : [];
    } catch {
      return [];
    }
  }
  const name = (r: RecipeInfo) => r.name[m.host.lang] || r.name.en || r.id;
  /**
   * Preview colours from the id (stable, no per-look assets) and motion from its tags.
   * The plugin never sends tags over obs-websocket (vendor-api.md 1.1), so motion
   * falls back to the id (cozy_chat → drift).
   */
  function hue(id: string): number {
    let h = 0;
    for (const c of id) h = (h * 31 + c.charCodeAt(0)) % 360;
    return h;
  }
  const hints = (r: RecipeInfo) => [...(r.tags ?? []), r.id];
  const motion = (r: RecipeInfo) => (hints(r).some((x) => /hype|sing|raid|karaoke/.test(x)) ? 'pulse' : hints(r).some((x) => /drift|chat|lofi|cozy/.test(x)) ? 'drift' : 'still');

  function apply(r: RecipeInfo) {
    if (m.host.streaming && !noAsk.includes(r.id)) {
      asking = r.id;
      skip = false;
      return;
    }
    void m.recipe(r.id);
  }
  function confirm(r: RecipeInfo) {
    if (skip) {
      noAsk = [...noAsk, r.id];
      try {
        localStorage.setItem(KEY, JSON.stringify(noAsk));
      } catch {
        /* storage blocked: asks again next time */
      }
    }
    asking = '';
    void m.recipe(r.id);
  }
</script>

<div class="looks" role="list">
  {#each m.recipes as r (r.id)}
    <div class="look" role="listitem">
      <span class="pv {motion(r)}" style="--h:{hue(r.id)}" aria-hidden="true"><span></span></span>
      <strong class="t">{name(r)}</strong>
      <span class="tags">
        {#if r.pro}<span class="badge pro">{t('stage.badge_pro')}</span>{/if}
        {#if r.user}<span class="badge">{t('stage.badge_user')}</span>{/if}
        {(r.tags ?? []).slice(0, 2).join(' · ')}
      </span>
      {#if asking === r.id}
        <div class="ask" role="group" aria-label={name(r)}>
          <span>{t('recipe.apply_confirm', { name: name(r), s: 1 })}</span>
          <label class="skip"><input type="checkbox" checked={skip} onchange={(e) => (skip = e.currentTarget.checked)} />{t('stage.dont_ask')}</label>
          <span class="row">
            <button type="button" class="b p" onclick={() => confirm(r)}>{t('stage.apply')}</button>
            <button type="button" class="b g" onclick={() => (asking = '')}>{t('stage.cancel')}</button>
          </span>
        </div>
      {:else}
        <button type="button" class="b" aria-label="{t('stage.apply')}: {name(r)}" onclick={() => apply(r)}>{t('stage.apply')}</button>
      {/if}
    </div>
  {/each}
</div>
<p class="note">{t('stage.look_howto')}</p>

<style>
  @layer screens {
    .looks { display: flex; gap: var(--sp-4); overflow-x: auto; padding-bottom: var(--sp-3); scroll-snap-type: x proximity; }
    .look {
      flex: 0 0 9.5rem;
      display: flex;
      flex-direction: column;
      gap: var(--sp-3);
      padding: var(--sp-4);
      border: 1px solid var(--c-border);
      border-radius: var(--r-lg);
      background: var(--c-surface-2);
      scroll-snap-align: start;
    }
    .pv {
      position: relative;
      aspect-ratio: 16 / 9;
      border-radius: var(--r-md);
      overflow: hidden;
      background: linear-gradient(135deg, hsl(var(--h) 60% 30%), hsl(calc(var(--h) + 60) 70% 55%));
    }
    .pv span {
      position: absolute;
      inset: 30% 38% 0;
      border-radius: 40% 40% 0 0;
      background: hsl(var(--h) 30% 85% / .85);
    }
    .look:hover .drift span, .look:focus-within .drift span { animation: drift 3s var(--ease-in-out) infinite alternate; }
    .look:hover .pulse span, .look:focus-within .pulse span { animation: pulse 0.8s var(--ease-out) infinite alternate; }
    @keyframes drift { to { translate: 12% -4%; scale: 1.06; } }
    @keyframes pulse { to { scale: 1.12; } }
    .t { font: var(--fw-bold) var(--fs-13) / var(--lh-tight) var(--font-display); }
    .tags { display: flex; flex-wrap: wrap; gap: var(--sp-2); color: var(--c-text-subtle); font-size: var(--fs-11); min-height: 1.2em; }
    .ask { display: flex; flex-direction: column; gap: var(--sp-3); font-size: var(--fs-12); }
    .skip { flex-direction: row; align-items: center; }
    .skip input { width: auto; height: auto; }
  }
</style>
