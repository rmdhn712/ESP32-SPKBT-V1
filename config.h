#pragma once

/**
 * ╔═════════════════════════════════════════════════════════════════════════════╗
 * ║               PANEL SETELAN — ESP32 BT Speaker                             ║
 * ╠═════════════════════════════════════════════════════════════════════════════╣
 * ║  Edit file ini untuk mengatur suara. Setelah simpan, Upload ke ESP32.       ║
 * ║  File ini adalah tab terpisah — tidak perlu menyentuh BT_Speaker.ino.       ║
 * ╚═════════════════════════════════════════════════════════════════════════════╝
 */


// ═════════════════════════════════════════════════════════════════════════════
//  NAMA BLUETOOTH
// ═════════════════════════════════════════════════════════════════════════════

#define BT_DEVICE_NAME   "ESP32-BT-Speaker"
//  Nama perangkat yang muncul di HP/PC saat pairing.
//  Ganti sesuai keinginan, max ~32 karakter.


// ═════════════════════════════════════════════════════════════════════════════
//  [1] STARTUP MUTE
//      Diam beberapa saat saat pertama konek Bluetooth.
//      Memberi waktu filter internal settling agar tidak ada dengung/pop.
// ═════════════════════════════════════════════════════════════════════════════

#define MUTE_MS  800
//  Satuan  : milidetik
//  Terlalu pendek → ada dengung/pop saat pertama konek
//  Terlalu panjang → awal lagu terpotong
//  Range wajar : 500 – 1500


// ═════════════════════════════════════════════════════════════════════════════
//  [2] NOISE GATE
//      Memotong hiss/desis saat tidak ada musik yang diputar.
// ═════════════════════════════════════════════════════════════════════════════

#define GATE_THRESHOLD  80
//  Satuan  : LSB (0 – 32767)
//  Level minimum sinyal agar gate terbuka dan suara bisa lewat.
//  Naik  → gate lebih agresif, hiss sedikit, tapi not pelan bisa ikut terpotong
//  Turun → gate lebih longgar, hiss sedikit lebih terdengar
//  Range wajar : 40 – 200

#define GATE_RELEASE_MS  120
//  Satuan  : milidetik
//  Berapa lama gate tetap terbuka setelah sinyal hilang.
//  Naik  → gate lambat menutup, tidak terasa "putus-putus" di sela not
//  Turun → gate cepat menutup, hiss cepat hilang tapi bisa "chattering"
//  Range wajar : 50 – 300


// ═════════════════════════════════════════════════════════════════════════════
//  [3] LOW-PASS FILTER
//      Memotong frekuensi tinggi yang terdengar tajam atau sember.
// ═════════════════════════════════════════════════════════════════════════════

#define LPF_FC_HZ  14000
//  Satuan  : Hz
//  Frekuensi di atas nilai ini akan diredam secara bertahap (12 dB/oktaf).
//  Naik  → suara lebih terang/detail, tapi bisa sember di volume tinggi
//  Turun → suara lebih warm/hangat, cocok untuk bass-heavy music
//  Range wajar : 8000 – 18000


// ═════════════════════════════════════════════════════════════════════════════
//  [4] COMPRESSOR
//      Menjaga volume tetap stabil. Saat sinyal keras, gain otomatis
//      dikurangi sementara. Hasilnya: volume tinggi tidak distorsi.
// ═════════════════════════════════════════════════════════════════════════════

#define COMP_THRESHOLD_PCT  20
//  Satuan  : % dari full scale (0 – 100)
//  Level di mana compressor mulai bekerja.
//  Naik  → kompresi mulai lebih lambat, transien lebih "punchy"
//  Turun → kompresi lebih agresif, volume lebih rata tapi dinamika berkurang
//  Range wajar : 10 – 40

#define COMP_RATIO  0.4f
//  Satuan  : 0.0 – 1.0 (bukan rasio biasa)
//  Seberapa kuat kompresi di atas threshold.
//    0.5  → rasio 2:1   (ringan, dinamika masih terasa)
//    0.4  → rasio 2.5:1 (sedang)
//    0.25 → rasio 4:1   (cukup keras)
//    0.1  → rasio 10:1  (hampir limiter)
//  Naik mendekati 1.0 → hampir tidak ada kompresi
//  Turun mendekati 0.0 → kompresi sangat keras, suara bisa flat/monoton
//  Range wajar : 0.2 – 0.6

#define COMP_ATTACK_MS  10
//  Satuan  : milidetik
//  Seberapa cepat compressor bereaksi saat sinyal tiba-tiba keras.
//  Naik  → transien (drum, perkusi) lebih terasa "punch", tapi bisa distorsi sesaat
//  Turun → transien langsung dikontrol, suara lebih "smooth"
//  Range wajar : 5 – 50

#define COMP_RELEASE_MS  150
//  Satuan  : milidetik
//  Seberapa cepat gain kembali normal setelah sinyal keras berlalu.
//  Naik  → gain pulih lebih lambat, leveling lebih halus
//  Turun → gain pulih cepat, bisa terdengar "pumping" pada musik lambat
//  Range wajar : 80 – 500


// ═════════════════════════════════════════════════════════════════════════════
//  [5] SOFT CLIPPER
//      Pelindung terakhir sebelum DAC. Membentuk sinyal yang terlalu keras
//      secara halus (cubic) sehingga tidak terdengar "krek" seperti hard clip.
// ═════════════════════════════════════════════════════════════════════════════

#define CLIP_THRESHOLD  20000
//  Satuan  : LSB (0 – 32767)
//  Level di mana soft clipper mulai membentuk sinyal.
//  Naik  → clipper jarang bekerja, suara lebih "open" tapi lebih berisiko krecek
//  Turun → clipper lebih aktif, suara lebih "saturated/compressed"
//  Range wajar : 16000 – 28000


// ═════════════════════════════════════════════════════════════════════════════
//  [6] OUTPUT GAIN
//      Volume keluaran akhir ke DAC.
// ═════════════════════════════════════════════════════════════════════════════

#define OUT_GAIN  2.0f
//  Satuan  : pengganda (1.0 = tidak ada perubahan, 2.0 = +6 dB)
//  Naik  → lebih keras. Jika terlalu tinggi, krecek meski ada compressor/clipper
//  Turun → lebih pelan, lebih aman dari distorsi
//  Range wajar (dengan compressor aktif) : 1.2 – 2.5
//
//  KOMBINASI PRESET:
//    Keras tapi bersih  → COMP_THRESHOLD_PCT 15, COMP_RATIO 0.3f, OUT_GAIN 2.5f
//    Sedang, dinamis    → COMP_THRESHOLD_PCT 25, COMP_RATIO 0.5f, OUT_GAIN 1.8f
//    Pelan, aman        → COMP_THRESHOLD_PCT 30, COMP_RATIO 0.5f, OUT_GAIN 1.2f
