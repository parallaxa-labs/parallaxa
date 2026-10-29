<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Standalone Stage dock (Parallaxa data/dock/dock-standalone.html; docs/ux/stage-plugin-ux.md
  section 7): the Stage control for Parallaxa without AI Live Studio, talking to obs-websocket
  directly. First run asks for the address and password once (kept in this OBS browser
  profile) and offers "Forget password". No AI features, never talks to AI Live Studio.
-->
<script lang="ts">
  import Layers from './Layers.svelte';
  import Manual from './Manual.svelte';
  import { DEFAULT_URL, type ObsHost } from './obs-host.svelte.ts';
  import StageControl from './StageControl.svelte';
  import { t } from './t.ts';

  let { host }: { host: ObsHost } = $props();
  const uid = $props.id();
  let url = $state.raw(host.settings.url);
  let password = $state.raw('');
  let show = $state.raw(false);
  $effect(() => {
    document.documentElement.lang = host.lang;
  });

  function connect(e: SubmitEvent) {
    e.preventDefault();
    host.save({ url: url.trim() || DEFAULT_URL, ...(password ? { password } : {}) });
    password = '';
    host.connect();
  }
</script>

<div class="sa">
  <header>
    <h1>Parallaxa</h1>
    {#if host.streaming}<span class="live">{t('stage.on_air')}</span>{/if}
  </header>
  <main>
    {#snippet settings(open: boolean)}
      <details class="card conn" {open}>
        <summary><h2>{t('stage_sa.connection')}</h2></summary>
        <form onsubmit={connect}>
          {#if host.conn === 'auth'}<p role="alert" class:err={host.authFailed}>{t(host.authFailed ? 'stage_sa.wrong' : 'stage_sa.auth')}</p>{/if}
          <label>
            <span>{t('stage_sa.url')}</span>
            <input value={url} oninput={(e) => (url = e.currentTarget.value)} placeholder={DEFAULT_URL} spellcheck="false" autocomplete="off" />
          </label>
          <label>
            <span>{t('stage_sa.password')}</span>
            <input id="{uid}-pw" type={show ? 'text' : 'password'} value={password} oninput={(e) => (password = e.currentTarget.value)} autocomplete="current-password" placeholder={host.settings.password ? '••••••••' : ''} />
          </label>
          <label class="show"><input type="checkbox" checked={show} onchange={(e) => (show = e.currentTarget.checked)} />{t('stage_sa.show')}</label>
          <div class="row">
            <button type="submit" class="b p">{t('stage_sa.connect')}</button>
            {#if host.settings.password}<button type="button" class="b g" onclick={() => host.forget()}>{t('stage_sa.forget')}</button>{/if}
          </div>
          <label>
            <span>{t('stage_sa.lang')}</span>
            <!-- {@attach}, not value=: see StageControl.svelte. -->
            <select {@attach (el) => void (el.value = host.settings.lang)} onchange={(e) => host.save({ lang: e.currentTarget.value as typeof host.settings.lang })}>
              <option value="auto">{t('stage_sa.lang_auto')}</option>
              <option value="en">English</option>
              <option value="ja">日本語</option>
              <option value="id">Bahasa Indonesia</option>
            </select>
          </label>
        </form>
      </details>
    {/snippet}
    {#if host.conn === 'auth'}
      <div class="stage">{@render settings(true)}</div>
    {:else}
      <StageControl {host} layout="dock" manual={() => Promise.resolve({ default: Manual })} layers={() => Promise.resolve({ default: Layers })}>
        {#snippet footer()}{@render settings(false)}{/snippet}
      </StageControl>
    {/if}
  </main>
</div>

<style>
  @layer screens {
    .sa { display: flex; flex-direction: column; height: 100vh; height: 100dvh; min-width: 0; }
    header {
      display: flex;
      align-items: center;
      gap: var(--sp-4);
      min-height: var(--dock-header-h);
      padding: 0 var(--sp-5);
      border-bottom: 1px solid var(--c-border);
      background: var(--c-surface-1);
    }
    h1 { flex: 1 1 auto; font-size: var(--fs-13); }
    .live { padding: 0 var(--sp-3); border-radius: var(--r-pill); background: var(--c-live); color: var(--c-on-live); font-size: var(--fs-11); font-weight: var(--fw-bold); }
    main { flex: 1 1 auto; min-height: 0; overflow: auto; padding: var(--sp-5); container-type: inline-size; }
    form { display: flex; flex-direction: column; gap: var(--sp-4); padding: 0 var(--sp-5) var(--sp-5); }
    .show { flex-direction: row; align-items: center; gap: var(--sp-3); }
    .show input { width: auto; height: auto; }
  }
</style>
