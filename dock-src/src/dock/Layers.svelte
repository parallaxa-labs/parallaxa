<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Layers and depth of field. Each scene item from layer_list gets a role select
  (layer_set role) and, for Background / Character / Foreground, a depth slider from -100 (far)
  to +100 (near), sent as depth = depthFromUi(v) = 10^(-v/100) (vendor-api.md 3), so every step
  of the travel moves the layer and ±100 is exactly depth 10..0.1. Visibility and lock are
  read-only icons; the character row has a star and blurred layers a blur mark.
  Depth of field: on/off (on = amount > 0), Focus in the same -100..100 units, Blur amount,
  and Rack focus (animated dof_set). Without any Stage Depth Blur filter the card explains it.
-->
<script lang="ts">
  import { ROLES, uiFromDepth, UI_DEPTH_MAX, UI_DEPTH_MIN, type Role } from '../lib/protocol/vendor.ts';
  import type { StageModel } from './model.svelte.ts';
  import Range from './Range.svelte';
  import { t } from './t.ts';

  let { m, part }: { m: StageModel; part: 'layers' | 'dof' } = $props();
  const ends = $derived<[string, string]>([t('stage.depth_far'), t('stage.depth_near')]);
  const blur = $derived(m.layers.some((l) => l.depth_blur));
  const dof = $derived(m.st?.dof);
  const dofOn = $derived((dof?.amount ?? 0) > 0);
  const DEPTH_ROLES: readonly Role[] = ['bg', 'char', 'fg'];
</script>

{#if part === 'layers'}
  {#if !m.layers.length}
    <p class="note">{t('stage.no_layers')}</p>
  {/if}
  <ul class="list">
    {#each m.layers as l (l.item_id)}
      <li>
        <div class="head">
          <span class="name" title={l.name}>{l.role === 'char' ? '★ ' : ''}{l.name}</span>
          {#if l.depth_blur}<span class="badge" title={t('stage.has_blur')}>{t('stage.blur_short')}</span>{/if}
          {#if !l.visible}<span class="badge" title={t('stage.hidden_item')}>{t('stage.hidden_short')}</span>{/if}
          {#if l.locked}<span class="badge" title={t('stage.locked_item')}>{t('stage.locked_short')}</span>{/if}
        </div>
        <label>
          <span class="sr-only">{t('stage.role')}: {l.name}</span>
          <!-- {@attach}, not value=: see StageControl.svelte. -->
          <select {@attach (el) => void (el.value = l.role)} disabled={m.locked} title={m.locked ? t('stage.locked_live') : undefined} onchange={(e) => m.role(l, e.currentTarget.value as Role)}>
            {#each ROLES as r (r)}<option value={r}>{t(`stage.role.${r}`)}</option>{/each}
          </select>
        </label>
        {#if DEPTH_ROLES.includes(l.role) && l.depth != null}
          <Range
            label="{t('stage.depth')}: {l.name}"
            value={Math.round(uiFromDepth(l.depth))}
            min={UI_DEPTH_MIN}
            max={UI_DEPTH_MAX}
            def={0}
            {ends}
            locked={m.locked} onair={m.host.streaming}
            oninput={(v) => m.depth(l, v)}
            onchange={(p) => m.remember(l.name, () => m.depth(l, p))}
          />
        {/if}
      </li>
    {/each}
  </ul>
  <button type="button" class="b" disabled={m.locked || !m.layers.length} title={m.locked ? t('stage.locked_live') : undefined} onclick={() => m.autoAssign()}>{t('stage.auto_assign')}</button>
{:else}
  {#if !blur}<p class="note">{t('stage.dof_needs_blur')}</p>{/if}
  <div class="row">
    <button
      type="button"
      class="b"
      role="switch"
      aria-checked={dofOn}
      disabled={!blur || m.locked}
      onclick={() => m.dof({ amount: dofOn ? 0 : m.dofRestore })}>{t('stage.dof')}: {t(dofOn ? 'stage.on' : 'stage.off')}</button
    >
    <button type="button" class="b" disabled={!blur} onclick={() => m.rack()}>{t('stage.dof_rack')}</button>
  </div>
  <div class="cols">
    <Range
      label={t('stage.dof_focus')}
      value={dof ? Math.round(uiFromDepth(dof.focus_depth)) : undefined}
      min={UI_DEPTH_MIN}
      max={UI_DEPTH_MAX}
      def={0}
      {ends}
      locked={m.locked} onair={m.host.streaming}
      disabled={!blur}
      oninput={(v) => m.dof({ focus: v })}
      onchange={(p) => m.remember(t('stage.dof_focus'), () => m.dof({ focus: p }))}
    />
    <Range
      label={t('stage.dof_amount')}
      unit="%"
      value={dof ? Math.round(dof.amount * 100) : undefined}
      min={0}
      max={100}
      locked={m.locked} onair={m.host.streaming}
      disabled={!blur}
      oninput={(v) => m.dof({ amount: v / 100 })}
      onchange={(p) => m.remember(t('stage.dof_amount'), () => m.dof({ amount: p / 100 }))}
    />
  </div>
{/if}

<style>
  @layer screens {
    .list { display: flex; flex-direction: column; gap: var(--sp-5); margin: 0 0 var(--sp-5); padding: 0; list-style: none; }
    li { display: grid; gap: var(--sp-3); padding-bottom: var(--sp-5); border-bottom: 1px solid var(--c-border); }
    .head { display: flex; align-items: center; gap: var(--sp-3); min-width: 0; }
    .name { flex: 1 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-weight: var(--fw-medium); color: var(--c-text); }
  }
</style>
