<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Effects: FX preset select (built-in ids,
  plus the current user preset shown by id), LUT select, the main sliders, pulse buttons and
  All effects off; the other sliders sit under "More effects". Protocol v1 cannot read FX
  values, so each slider shows the value sent from here, else the preset's, else "unknown".
-->
<script lang="ts">
  import { PULSE_EFFECTS, type FxParams } from '../lib/protocol/vendor.ts';
  import { FX_PRESETS, FX_SLIDERS, fxValue, LUTS, type FxSlider } from './fx.ts';
  import type { StageModel } from './model.svelte.ts';
  import Range from './Range.svelte';
  import { t } from './t.ts';

  let { m, dock }: { m: StageModel; dock: boolean } = $props();
  const preset = $derived(m.st?.fx.preset ?? '');
  const userPreset = $derived(preset && !(preset in FX_PRESETS) ? preset : '');
  const val = (p: keyof FxParams) => fxValue(p, m.fxSent, m.fxBase);
  const lut = $derived(val('lut') as string | undefined);
  const main = FX_SLIDERS.filter((s) => s.main);
  const more = FX_SLIDERS.filter((s) => !s.main);
  const label = (s: FxSlider) => `${t(s.g === 'colour' ? 'stage.fx_group.colour' : `stage.fx.${s.g}`)}: ${t(`stage.fx_param.${s.p}`)}`;
</script>

{#snippet slider(s: FxSlider)}
  {@const v = val(s.p) as number | undefined}
  <Range
    label={label(s)}
    unit={s.unit}
    value={v === undefined ? undefined : Math.round(v * s.scale * 100) / 100}
    min={s.min * s.scale}
    max={s.max * s.scale}
    step={s.step}
    def={s.neutral * s.scale}
    locked={m.locked} onair={m.host.streaming}
    disabled={m.noFx}
    oninput={(x) => m.fx(s.p, x / s.scale)}
    onchange={(p) => m.remember(t(`stage.fx_param.${s.p}`), () => m.fx(s.p, p / s.scale))}
  />
{/snippet}

{#if m.noFx}<p class="note">{t('stage.no_fx')}</p>{/if}
<p class="note">{t('stage.fx_values_note')}</p>
<div class="cols">
  <label>
    <span>{t('stage.fx_preset')}</span>
    <!-- {@attach}, not value=: see StageControl.svelte. -->
    <select {@attach (el) => void (el.value = preset)} disabled={m.noFx} onchange={(e) => m.fxPreset(e.currentTarget.value)}>
      {#if !preset}<option value="" disabled>—</option>{/if}
      {#each Object.keys(FX_PRESETS) as id (id)}<option value={id}>{t(`stage.fx_preset.${id}`)}</option>{/each}
      {#if userPreset}<option value={userPreset}>{userPreset} ({t('stage.badge_user')})</option>{/if}
    </select>
  </label>
  <label>
    <span>{t('stage.fx.lut')}</span>
    <select {@attach (el) => void (el.value = lut ?? '?')} disabled={m.noFx || m.locked} onchange={(e) => m.fx('lut', e.currentTarget.value)}>
      {#if lut === undefined}<option value="?" disabled>—</option>{/if}
      {#each LUTS as id (id)}<option value={id}>{t(`stage.lut.${id || 'none'}`)}</option>{/each}
    </select>
  </label>
  {#each main as s (s.p)}{@render slider(s)}{/each}
</div>
<p class="note">{t('stage.presets_in_obs')}</p>
<div class="row" role="group" aria-label={t('stage.pulse')}>
  {#each PULSE_EFFECTS as e (e)}
    <button type="button" class="b" disabled={m.noFx} onclick={() => m.pulse(e)}>{t(`stage.pulse.${e}`)}</button>
  {/each}
  <button type="button" class="b" disabled={m.noFx} onclick={() => m.fxPreset('neutral')}>{t('stage.fx_all_off')}</button>
</div>
<p class="note">{t('stage.flash_cap')}</p>
<details open={!dock}>
  <summary>{t('stage.fx_more')}</summary>
  <div class="cols">
    {#each more as s (s.p)}{@render slider(s)}{/each}
  </div>
  <p class="note">{t('stage.leak_in_obs')}</p>
</details>
