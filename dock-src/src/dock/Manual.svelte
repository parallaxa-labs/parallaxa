<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Manual camera: a drag pad (drag = pan, wheel = zoom, Shift+drag = roll) with the
  sliders mirrored below. While dragging it sends camera_set with duration_ms 0 at 30 Hz and
  one eased camera_set on release (vendor-api.md 4.4). Arrow keys on the pad nudge with a
  relative camera_set (Shift = bigger steps), +/- zoom, [ and ] roll. All of it is
  picture-moving, so the on-air lock applies: a lock that engages mid-drag ends the drag
  where it is (no eased release), and while locked the wheel scrolls the dock.
-->
<script lang="ts">
  import { ROT_MAX, ROT_MIN, ZOOM_MAX, ZOOM_MIN, type CameraPose } from '../lib/protocol/vendor.ts';
  import { throttle } from '../lib/ui/throttle.ts';
  import { EASE_RELEASE, type StageModel } from './model.svelte.ts';
  import Range from './Range.svelte';
  import { t } from './t.ts';

  let { m }: { m: StageModel } = $props();
  const uid = $props.id();
  const HOME: CameraPose = { x: 0, y: 0, zoom: 1, rot: 0 };
  const clamp = (v: number, a: number, b: number) => Math.min(b, Math.max(a, v));

  const W = $derived(m.info?.canvas.width ?? 1920);
  const H = $derived(m.info?.canvas.height ?? 1080);
  /** The pose shown: the local one while dragging, else the rig's. */
  let drag = $state.raw<CameraPose | null>(null);
  const pose = $derived(drag ?? m.st?.camera ?? HOME);

  let from: { pose: CameraPose; px: number; el: HTMLElement } | null = null;
  const send = throttle((p: Partial<CameraPose>) => void m.camera(p, 0), 33);
  $effect(() => () => send.cancel());

  function undoable(prev: CameraPose) {
    m.remember(t('stage.manual'), () => m.camera(prev, 300, EASE_RELEASE));
  }

  function at(e: PointerEvent, f: NonNullable<typeof from>): CameraPose {
    const r = f.el.getBoundingClientRect();
    const base = f.pose;
    if (e.shiftKey) return { ...base, rot: Math.round(clamp(base.rot + ((e.clientX - f.px) / r.width) * 90, ROT_MIN, ROT_MAX)) };
    return {
      ...base,
      x: Math.round(clamp((e.clientX - r.left) / r.width - 0.5, -0.5, 0.5) * W),
      y: Math.round(clamp((e.clientY - r.top) / r.height - 0.5, -0.5, 0.5) * H),
    };
  }
  function down(e: PointerEvent) {
    if (e.button !== 0 || !m.free()) return;
    const el = e.currentTarget as HTMLElement;
    el.setPointerCapture?.(e.pointerId);
    from = { pose: { ...pose }, px: e.clientX, el };
    move(e);
  }
  function move(e: PointerEvent) {
    if (!from) return;
    if (m.locked) return up(e);
    drag = at(e, from);
    const { x, y, rot } = drag;
    send(e.shiftKey ? { rot } : { x, y });
  }
  function up(e: PointerEvent) {
    if (!from) return;
    send.cancel();
    // The lock engaged mid-drag: keep what already went out, send nothing more.
    if (!m.locked) void m.camera(at(e, from), 250, EASE_RELEASE);
    undoable(from.pose);
    from = null;
    drag = null;
  }

  // Wheel zoom: one undo step per burst.
  let wheelFrom: CameraPose | null = null;
  let wheelEnd: ReturnType<typeof setTimeout> | undefined;
  function wheel(e: WheelEvent) {
    if (m.locked) return; // let the dock scroll
    e.preventDefault();
    wheelFrom ??= { ...pose };
    const zoom = Math.round(clamp(pose.zoom * (1 - e.deltaY * 0.001), ZOOM_MIN, ZOOM_MAX) * 100) / 100;
    drag = { ...pose, zoom };
    send({ zoom });
    clearTimeout(wheelEnd);
    wheelEnd = setTimeout(() => {
      if (wheelFrom) undoable(wheelFrom);
      wheelFrom = drag = null;
    }, 400);
  }

  function key(e: KeyboardEvent) {
    const big = e.shiftKey ? 5 : 1;
    const step = Math.round(W * 0.02) * big;
    const k: Record<string, Partial<CameraPose>> = {
      ArrowLeft: { x: -step },
      ArrowRight: { x: step },
      ArrowUp: { y: -step },
      ArrowDown: { y: step },
      '+': { zoom: 0.05 * big },
      '=': { zoom: 0.05 * big },
      '-': { zoom: -0.05 * big },
      '[': { rot: -big },
      ']': { rot: big },
    };
    const d = k[e.key];
    if (!d || e.ctrlKey || e.metaKey) return;
    e.preventDefault();
    m.nudge(d);
  }

  /** Pose sliders: live while dragging (Range sends), the commit is undoable. */
  const live = (k: keyof CameraPose, v: number) => void m.camera({ [k]: v }, 0);
  const commit = (k: keyof CameraPose, prev: number) => m.remember(t('stage.manual'), () => m.camera({ [k]: prev }, 300, EASE_RELEASE));
</script>

<button
  type="button"
  class="pad {m.locked ? 'off' : ''}"
  aria-label={t('stage.manual')}
  aria-describedby="{uid}-h"
  aria-disabled={m.locked || undefined}
  onpointerdown={down}
  onpointermove={move}
  onpointerup={up}
  onpointercancel={up}
  onwheel={wheel}
  onkeydown={key}
>
  <span class="view" style="left:{(pose.x / W + 0.5) * 100}%;top:{(pose.y / H + 0.5) * 100}%;width:{100 / pose.zoom}%;height:{100 / pose.zoom}%;rotate:{pose.rot}deg"></span>
  <span class="dot" style="left:{(pose.x / W + 0.5) * 100}%;top:{(pose.y / H + 0.5) * 100}%"></span>
</button>
<p id="{uid}-h" class="note">{t('stage.pad_hint')}</p>
<!-- Home in this card too (not locked on air, like the Camera card's). -->
<button type="button" class="b" onclick={() => m.home()}>{t('stage.home')}</button>
<div class="cols">
  <Range label={t('stage.pan_x')} unit="px" value={pose.x} min={-W / 2} max={W / 2} def={0} locked={m.locked} onair={m.host.streaming} oninput={(v) => live('x', v)} onchange={(p) => commit('x', p)} />
  <Range label={t('stage.pan_y')} unit="px" value={pose.y} min={-H / 2} max={H / 2} def={0} locked={m.locked} onair={m.host.streaming} oninput={(v) => live('y', v)} onchange={(p) => commit('y', p)} />
  <Range label={t('stage.zoom')} unit="x" value={pose.zoom} min={ZOOM_MIN} max={ZOOM_MAX} step={0.01} def={1} locked={m.locked} onair={m.host.streaming} oninput={(v) => live('zoom', v)} onchange={(p) => commit('zoom', p)} />
  <Range label={t('stage.roll')} unit="°" value={pose.rot} min={ROT_MIN} max={ROT_MAX} def={0} locked={m.locked} onair={m.host.streaming} oninput={(v) => live('rot', v)} onchange={(p) => commit('rot', p)} />
</div>

<style>
  @layer screens {
    .pad {
      position: relative;
      display: block;
      width: 100%;
      padding: 0;
      aspect-ratio: 16 / 9;
      border: 1px solid var(--c-border-strong);
      border-radius: var(--r-md);
      background:
        linear-gradient(var(--c-border) 1px, transparent 1px) 0 50% / 100% 50%,
        linear-gradient(90deg, var(--c-border) 1px, transparent 1px) 50% 0 / 50% 100%,
        var(--c-surface-2);
      overflow: hidden;
      touch-action: none;
      cursor: crosshair;
    }
    .pad.off { opacity: .45; cursor: not-allowed; }
    .view, .dot { position: absolute; translate: -50% -50%; pointer-events: none; }
    .view { border: 1px solid var(--c-primary); border-radius: var(--r-xs); background: var(--c-primary-tint); }
    .dot { width: 12px; height: 12px; border-radius: 50%; background: var(--c-primary); box-shadow: 0 0 0 2px var(--c-bg); }
  }
</style>
