# Parallaxa LUT strips

Original procedural LUTs, Copyright (C) 2026 Parallaxa Labs, GPL-2.0-or-later with the plugin. No third-party LUTs.

## Layout (all files)

512x512 RGB PNG holding a 64x64x64 LUT as 8x8 tiles of 64x64 pixels:

```
tile   = blue index b (0..63): column b % 8, row b / 8
inside = red index r (0..63) along x, green index g (0..63) along y (top = 0)
pixel (x, y): r = x % 64, g = y % 64, b = (y / 64) * 8 + x / 64
input colour = (r, g, b) / 63; stored output = round(f(input) * 255)
```

A shader samples it with the blue slice pair (`floor`/`ceil` of `b * 63`), each tile offset `((b % 8) * 64, (b / 8) * 64)` plus `(r * 63 + 0.5, g * 63 + 0.5)` pixels, and lerps between the two slices. `identity.png` must reproduce its input exactly (checked by `tests/contracts/presets_check.mjs`).

## Parameters

All maths in normalised RGB (0..1), clamped to 0..1 at the end of each step that can leave the range. `luma = 0.2126 R + 0.7152 G + 0.0722 B`; `sat(c, s) = luma + (c - luma) * s`; `smoothstep(v) = v^2 (3 - 2v)`.

### `identity`

`f(c) = c`.

### `film` (warm filmic, used by FX presets `film` and `lofi`)

1. Lift blacks: `c = 0.04 + 0.96 c`.
2. Soft S-curve: `c = mix(c, smoothstep(c), 0.35)` per channel.
3. Split toning with `l = luma(c)`, `sh = (1 - l)^2`, `hi = l^2`:
   `R += -0.02 sh + 0.04 hi`, `G += 0.01 sh + 0.015 hi`, `B += 0.035 sh - 0.03 hi` (teal shadows, warm highlights).
4. Saturation 0.9: `c = sat(c, 0.9)`, clamp.

### `night` (cool moonlight, used by FX presets `horror` and `rainy`)

1. Saturation 0.55: `c = sat(c, 0.55)`.
2. Cool balance: `R *= 0.88`, `G *= 0.97`, `B *= 1.10`.
3. Gamma 1.12 (darker mid-tones): `c = clamp(c)^1.12`.
4. Teal-green deep shadows with `t = (1 - luma(c))^3`: `G += 0.015 t`, `B += 0.02 t`.
5. Crush blacks: `c = clamp((c - 0.02) / 0.98)`.
