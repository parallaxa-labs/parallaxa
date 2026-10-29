# Panduan pengguna Parallaxa

Parallaxa adalah plugin OBS Studio gratis (GPL) untuk VTuber 2D: kedalaman dan parallax antar lapisan scene, gerakan kamera virtual (zoom, pan, rotasi, getar, drift otomatis, hentakan reaktif audio), tampilan warna, efek, dan blur kedalaman per lapisan. Plugin ini bekerja sendiri dengan sumber avatar apa pun (VTube Studio lewat Spout2, gambar PNGTuber, webcam). Aplikasi seperti AI Live Studio dapat mengendalikannya dari jauh lewat obs-websocket (`docs/protocol/vendor-api.md`).

English: [../en/guide.md](../en/guide.md) · 日本語: [../ja/guide.md](../ja/guide.md)

## 1. Yang Anda perlukan

- Windows 10 (1803 atau lebih baru) atau 11, 64-bit
- OBS Studio 32.0 atau lebih baru (Steam atau installer dari obsproject.com)

## 2. Instalasi

1. **Tutup OBS.**
2. Jalankan `Parallaxa-<versi>-Setup.exe` dan setujui permintaan administrator.
3. Di halaman **OBS Studio**, periksa foldernya. Setup mencari versi installer resmi, library Steam, dan `C:\Program Files\obs-studio`. Jika Anda memakai salinan lain, klik **Browse** dan pilih folder yang berisi `bin\64bit\obs64.exe`.
4. Setup membaca versi OBS lalu memilih tata letak plugin:
   - **OBS 33 atau lebih baru:** `C:\ProgramData\obs-studio\plugins\parallaxa\parallaxa.dll`
   - **OBS 32:** `C:\ProgramData\obs-studio\plugins\parallaxa\bin\64bit\parallaxa.dll`
   - **OBS tidak ditemukan:** keduanya (OBS 33+ memakai yang pertama dan mengabaikan yang kedua).

   Di OBS 32, **Also install the copy for OBS 33 and later** sudah tercentang secara bawaan, jadi kedua salinan diinstal dan plugin tetap termuat setelah Steam memperbarui OBS ke 33.
5. Jalankan OBS. Untuk memeriksa, buka **Help > Log Files > View Current Log** dan cari `[parallaxa] loaded`.

**Instal manual (zip).** Ekstrak `Parallaxa-<versi>-windows-x64.zip` ke `C:\ProgramData\obs-studio\plugins\` sehingga ada `...\plugins\parallaxa\`. Zip ini berisi kedua tata letak, jadi bisa dipakai di OBS 32 maupun 33.

## 3. Scene pertama dalam 10 detik

Di OBS pilih **Tools > Parallaxa: Siapkan scene saat ini**. Parallaxa menambahkan filter **Parallaxa Camera Rig** dan **Parallaxa FX** ke scene yang sedang aktif. Peran tiap item ditebak dari namanya: `bg`/`background` menjadi latar belakang, `vts`/`spout`/`avatar`/`model` menjadi karakter, `fg`/`foreground` menjadi latar depan. Lalu tampilan Cozy Chat diterapkan. Atur kedalaman tiap lapisan di properti filter. Untuk depth of field, tambahkan **Parallaxa Depth Blur** ke sumber latar belakang atau latar depan.

Untuk mencoba scene siap pakai, pilih **Tools > Parallaxa: Impor scene contoh**. Scene **Parallaxa Sample - Just Chatting** (gambar latar belakang dan latar depan orisinal, sumber Spout2 VTube Studio, filter Parallaxa) ditambahkan ke koleksi scene yang sedang dipakai. Gambarnya dimuat dari folder data plugin, jadi OBS tidak menanyakan file yang hilang. Kalau sudah ada scene atau sumber dengan nama yang sama, yang baru diberi nomor, misalnya "Background 2"; tidak ada yang tertimpa. Koleksi yang sama juga ada di `data\presets\samples\parallaxa_sample.json` untuk **Scene Collection > Import**.

## 4. Dock Parallaxa

Dock ini mengendalikan kamera, tampilan, efek, dan lapisan tanpa membuka properti filter. Tambahkan di **Docks > Custom Browser Docks** dengan URL ini:

```
file:///C:/ProgramData/obs-studio/plugins/parallaxa/data/dock/dock-standalone.html
```

Dock berkomunikasi langsung dengan OBS lewat obs-websocket. Nyalakan dulu WebSocket server (**Tools > WebSocket Server Settings**, port 4455); dock menanyakan kata sandinya satu kali.

## 5. VTube Studio lewat Spout2

1. Instal **plugin OBS Spout2** dari Off World Live (https://github.com/Off-World-Live/obs-spout2-plugin/releases) sesuai versi OBS Anda (OBS 32: **1.13.0**, OBS 33 atau lebih baru: **1.14.0**).
2. Di pengaturan VTube Studio, nyalakan **Spout2 output**.
3. Di OBS, tambahkan sumber **Spout2 Capture** dan pilih VTube Studio. Beri nama `VTS` atau `Avatar` agar **Parallaxa: Siapkan scene saat ini** mengenalinya sebagai karakter.
4. **Jalankan VTube Studio dan OBS di GPU yang sama.** Jika PC punya grafis terintegrasi sekaligus kartu NVIDIA atau AMD, sumber Spout2 bisa tetap hitam. Buka **Settings > System > Display > Graphics** di Windows, atur `VTube Studio.exe` dan `obs64.exe` ke **High performance**, lalu jalankan ulang keduanya.

## 6. Pemecahan masalah

| Masalah | Yang perlu dilakukan |
|---|---|
| Filter Parallaxa tidak ada di OBS | Pastikan OBS tertutup saat setup. Cari `parallaxa` di log OBS: `MODULE_INCOMPATIBLE_VER` berarti OBS Anda lebih lama dari 32.0. Jika OBS diperbarui dari 32 ke 33 dan centang salinan OBS 33 tadinya dihapus, jalankan lagi installer-nya. |
| Setup menampilkan "OBS Studio is running" | Tutup OBS, termasuk ikon tray, lalu klik **Retry**. |
| Dock tidak bisa terhubung | Nyalakan WebSocket server di OBS, periksa port 4455, dan masukkan lagi kata sandinya di dock. |
| Sumber Spout2 tetap hitam | Atur VTube Studio dan OBS ke GPU yang sama (bagian 5, langkah 4). Periksa versi plugin Spout2 untuk OBS Anda. |

## 7. Menghapus instalasi

Tutup OBS, lalu hapus "Parallaxa (OBS plugin)" di **Settings > Apps > Installed apps**. Agar aman, sebelumnya pilih **Tools > Parallaxa: Kembalikan semua posisi dasar** di OBS agar item scene kembali ke posisi yang tersimpan.

## 8. Lisensi

Parallaxa adalah perangkat lunak bebas dengan lisensi GNU GPL v2 atau lebih baru (`LICENSE.txt` di folder plugin). Lokasi kode sumber lengkapnya tertulis di `SOURCE.txt`; setiap rilis menerbitkan `Parallaxa-<versi>-source.zip` bersama binarinya. Komponen pihak ketiga tercantum di `THIRD_PARTY_NOTICES.md`.
