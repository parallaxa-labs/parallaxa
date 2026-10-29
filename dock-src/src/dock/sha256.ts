// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal SHA-256 (FIPS 180-4) for obs-websocket v5 authentication in the
// standalone dock. A file:// page is not a secure context, so SubtleCrypto may
// be missing there; this is about 0.6 KB minified and synchronous.

const K: number[] = [];
const H0: number[] = [];
{
  // First 32 bits of the fractional parts of the cube roots (K) and square roots (H0) of the first primes.
  const frac = (x: number) => ((x - Math.floor(x)) * 2 ** 32) | 0;
  for (let n = 2, i = 0; i < 64; n++) {
    let prime = true;
    for (let d = 2; d * d <= n; d++) if (n % d === 0) prime = false;
    if (!prime) continue;
    if (i < 8) H0.push(frac(n ** 0.5));
    K.push(frac(n ** (1 / 3)));
    i++;
  }
}

/** SHA-256 of bytes. */
export function sha256(data: Uint8Array): Uint8Array {
  const len = data.length;
  const blocks = ((len + 9 + 63) >> 6) << 6;
  const m = new Uint8Array(blocks);
  m.set(data);
  m[len] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(blocks - 4, len * 8); // messages here are far below 512 MB
  dv.setUint32(blocks - 8, Math.floor(len / 0x20000000));
  const h = H0.slice();
  const w = new Array<number>(64);
  const rotr = (x: number, n: number) => (x >>> n) | (x << (32 - n));
  for (let o = 0; o < blocks; o += 64) {
    for (let i = 0; i < 64; i++) {
      if (i < 16) w[i] = dv.getUint32(o + i * 4);
      else {
        const a = w[i - 15] as number;
        const b = w[i - 2] as number;
        w[i] = ((w[i - 16] as number) + (rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3)) + (w[i - 7] as number) + (rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10))) | 0;
      }
    }
    let [a, b, c, d, e, f, g, k] = h as [number, number, number, number, number, number, number, number];
    for (let i = 0; i < 64; i++) {
      const t1 = (k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + (K[i] as number) + (w[i] as number)) | 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      k = g;
      g = f;
      f = e;
      e = (d + t1) | 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) | 0;
    }
    [a, b, c, d, e, f, g, k].forEach((x, i) => (h[i] = ((h[i] as number) + x) | 0));
  }
  const out = new Uint8Array(32);
  const ov = new DataView(out.buffer);
  h.forEach((x, i) => ov.setUint32(i * 4, x));
  return out;
}

/** base64(sha256(utf8(text))). */
export function sha256b64(text: string): string {
  return btoa(String.fromCharCode(...sha256(new TextEncoder().encode(text))));
}

/**
 * obs-websocket v5 `authentication` string (Identify, op 1):
 * base64(sha256(base64(sha256(password + salt)) + challenge)).
 */
export function obsAuth(password: string, salt: string, challenge: string): string {
  return sha256b64(sha256b64(password + salt) + challenge);
}
