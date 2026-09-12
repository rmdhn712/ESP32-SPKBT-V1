/**
 * =============================================================================
 * ESP32 Bluetooth A2DP Speaker — Internal DAC, Maximum Quality DSP
 * =============================================================================
 *
 * TARGET BOARD  : ESP32 DevKit V1 (ESP32-WROOM-32)
 * AMPLIFIER     : PAM8406 (Class D 3W+3W) dengan potensio volume onboard
 * LIBRARIES     : ESP32-A2DP v1.8.x + audio-tools (Phil Schatzmann)
 * CORE          : arduino-esp32 v2.x  (JANGAN pakai v3.x)
 *
 * PARTITION SCHEME : Huge APP (3MB No OTA / 1MB SPIFFS)
 * BOARD SETTINGS   : ESP32 Dev Module | CPU 240MHz | QIO | 4MB
 *
 * PIN OUTPUT:
 *   GPIO 25 → Left  channel (DAC1) → RC 470Ω+10nF → PAM8406 INL+
 *   GPIO 26 → Right channel (DAC2) → RC 470Ω+10nF → PAM8406 INR+
 *
 * =============================================================================
 * KENAPA driver/i2s.h TETAP DIPAKAI UNTUK DAC INTERNAL
 * =============================================================================
 *
 *  ESP32 internal DAC dengan I2S_MODE_DAC_BUILT_IN membutuhkan format data
 *  yang spesifik — TIDAK sama dengan int16_t signed biasa dari A2DP:
 *
 *    • Hanya 8 bit MSB setiap word 16-bit yang dipakai DAC
 *    • Data harus UNSIGNED: dac_val = (int16_sample >> 8) + 128
 *    • Terjadi channel SWAP di hardware: L dan R perlu di-pre-swap di software
 *
 *  audio_tools::I2SStream (signal_type=Analog) meneruskan int16 raw tanpa
 *  konversi ini → output DAC salah → silence atau noise.
 *
 *  Solusi: pakai driver/i2s.h hanya untuk install + write, DSP tetap via
 *  AudioEffectStreamT dari audio-tools.
 *
 * =============================================================================
 * DSP CHAIN (AudioEffectStreamT, per sample per channel):
 *   [1] DCBlockEffect     HPF fc=20Hz    anti dengung
 *   [2] NoiseGateEffect   thr=80 LSB     anti hiss idle
 *   [3] TwoPoleLPFEffect  fc=14kHz 12dB  anti sember
 *   [4] Compressor        thr=20% r=2.5  anti distorsi volume tinggi
 *   [5] SoftClipEffect    cubic C¹       safety net clipping
 *   [6] Boost (built-in)  x2.0 +6dB     volume output
 *   [7] DitherEffect      TPDF ±0.7LSB  anti hiss granuler
 * =============================================================================
 */

#include "config.h"             // ← SETELAN SUARA — edit file config.h
#include "AudioTools.h"         // AudioEffectStreamT, RingBufferStream, Boost
#include "BluetoothA2DPSink.h"  // A2DP
#include "driver/i2s.h"         // DAC internal ESP32


// ─────────────────────────────────────────────────────────────────────────────
// Konstanta derived dari config.h (jangan diedit di sini)
// ─────────────────────────────────────────────────────────────────────────────
static const i2s_port_t  I2S_NUM     = I2S_NUM_0;
static const uint32_t    MUTE_FRAMES = (uint32_t)(MUTE_MS * 44.1f);

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration
// ─────────────────────────────────────────────────────────────────────────────
void process_dsp(const uint8_t *data, uint32_t length);


// =============================================================================
// DSP Effect classes
// =============================================================================

// ── Stage 1 : DC Blocker (HPF fc≈20Hz) ───────────────────────────────────────
class DCBlockEffect : public audio_tools::AudioEffect {
public:
    DCBlockEffect() = default;
    DCBlockEffect(const DCBlockEffect &o) : xp(o.xp), yp(o.yp) {
        copyParent((audio_tools::AudioEffect *)&o);
    }
    audio_tools::effect_t process(audio_tools::effect_t in) override {
        float x = (float)in;
        float y = x - xp + R * yp;
        xp = x;  yp = y;
        return clip((int32_t)y);
    }
    DCBlockEffect *clone() override { return new DCBlockEffect(*this); }
private:
    // R = 1 - (2π × 20Hz / 44100Hz). Tidak perlu diubah — fc 20Hz optimal.
    static constexpr float R = 0.99715f;
    float xp = 0.0f, yp = 0.0f;
};

// ── Stage 2 : Noise Gate ──────────────────────────────────────────────────────
// SMOOTH = 0.01 → gate terbuka dalam ~100 sample (~2ms) saat ada sinyal.
class NoiseGateEffect : public audio_tools::AudioEffect {
public:
    NoiseGateEffect() = default;
    NoiseGateEffect(const NoiseGateEffect &o) : env(o.env), gain(o.gain) {
        copyParent((audio_tools::AudioEffect *)&o);
    }
    audio_tools::effect_t process(audio_tools::effect_t in) override {
        float ax = (float)(in < 0 ? -in : in);
        env = (ax > env) ? ax : env * RELEASE_TC;
        float target = (env >= THRESHOLD) ? 1.0f : 0.0f;
        gain += SMOOTH * (target - gain);
        return (audio_tools::effect_t)((float)in * gain);
    }
    NoiseGateEffect *clone() override { return new NoiseGateEffect(*this); }
private:
    // Nilai dari panel setelan: GATE_THRESHOLD dan GATE_RELEASE_MS
    static constexpr float THRESHOLD  = (float)GATE_THRESHOLD;
    // Release TC: semakin dekat ke 1.0, semakin lambat gate menutup
    // Rumus: TC = exp(-1 / (releaseMs * 44.1)) ≈ 1 - 1/(releaseMs*44.1)
    static constexpr float RELEASE_TC = 1.0f - (1.0f / (GATE_RELEASE_MS * 44.1f));
    static constexpr float SMOOTH     = 0.01f;  // kecepatan ramp gain (tidak perlu diubah)
    float env = 0.0f, gain = 0.0f;
};

// ── Stage 3 : Two-Pole Butterworth LPF (fc=14kHz, 12dB/oct) ──────────────────
// LowPassFilter<float> copy=delete → implementasi biquad DF2 manual.
class TwoPoleLPFEffect : public audio_tools::AudioEffect {
public:
    TwoPoleLPFEffect() {
        // Frekuensi cutoff dari panel setelan: LPF_FC_HZ
        const float w0    = 6.28318530718f * (float)LPF_FC_HZ / 44100.0f;
        const float cw    = cosf(w0);
        const float sw    = sinf(w0);
        const float alpha = sw / (2.0f * 0.7071f);
        const float a0    = 1.0f + alpha;
        b0 = (1.0f - cw) * 0.5f / a0;
        b1 = (1.0f - cw)        / a0;
        b2 = b0;
        a1 = (-2.0f * cw)       / a0;
        a2 = (1.0f - alpha)     / a0;
    }
    TwoPoleLPFEffect(const TwoPoleLPFEffect &o)
        : b0(o.b0), b1(o.b1), b2(o.b2), a1(o.a1), a2(o.a2),
          w1_0(o.w1_0), w1_1(o.w1_1), w2_0(o.w2_0), w2_1(o.w2_1) {
        copyParent((audio_tools::AudioEffect *)&o);
    }
    audio_tools::effect_t process(audio_tools::effect_t in) override {
        float x  = (float)in;
        float y1 = b0*x + w1_0;  w1_0 = b1*x - a1*y1 + w1_1;  w1_1 = b2*x - a2*y1;
        float y2 = b0*y1 + w2_0; w2_0 = b1*y1 - a1*y2 + w2_1; w2_1 = b2*y1 - a2*y2;
        return clip((int32_t)y2);
    }
    TwoPoleLPFEffect *clone() override { return new TwoPoleLPFEffect(*this); }
private:
    float b0=0, b1=0, b2=0, a1=0, a2=0;
    float w1_0=0, w1_1=0, w2_0=0, w2_1=0;
};

// ── Stage 4 : Soft Clipper (cubic C¹) ────────────────────────────────────────
class SoftClipEffect : public audio_tools::AudioEffect {
public:
    SoftClipEffect() = default;
    SoftClipEffect(const SoftClipEffect &o) {
        copyParent((audio_tools::AudioEffect *)&o);
    }
    audio_tools::effect_t process(audio_tools::effect_t in) override {
        return clip((int32_t)sc((float)in));
    }
    SoftClipEffect *clone() override { return new SoftClipEffect(*this); }
private:
    // Nilai dari panel setelan: CLIP_THRESHOLD
    // T2 = 2×T (batas atas cubic knee), TC = 4/3×T (ceiling output)
    static constexpr float T  = (float)CLIP_THRESHOLD;
    static constexpr float T2 = 2.0f * T;
    static constexpr float TC = (4.0f / 3.0f) * T;
    static float sc(float x) {
        float ax = x < 0 ? -x : x;
        float sg = x < 0 ? -1.f : 1.f;
        if (ax <= T)  return x;
        if (ax <= T2) { float u=(ax-T)/T; return sg*(T+T*(u-u*u*u/3.f)); }
        return sg*TC;
    }
};

// ── Stage 6 : TPDF Dither ────────────────────────────────────────────────────
class DitherEffect : public audio_tools::AudioEffect {
public:
    DitherEffect() = default;
    DitherEffect(const DitherEffect &o) : r1(o.r1), r2(o.r2) {
        copyParent((audio_tools::AudioEffect *)&o);
    }
    audio_tools::effect_t process(audio_tools::effect_t in) override {
        r1 = r1 * 1664525UL  + 1013904223UL;
        r2 = r2 * 22695477UL + 1UL;
        float d = ((float)(int32_t)r1 - (float)(int32_t)r2)
                  * (0.7f / 2147483648.0f);
        return clip((int32_t)((float)in + d));
    }
    DitherEffect *clone() override { return new DitherEffect(*this); }
private:
    uint32_t r1 = 0x12345678UL, r2 = 0x87654321UL;
};


// ─────────────────────────────────────────────────────────────────────────────
// Global objects (definisi di sini, sebelum dipakai)
// ─────────────────────────────────────────────────────────────────────────────
static DCBlockEffect             dcBlock;
static NoiseGateEffect           noiseGate;
static TwoPoleLPFEffect          twoPoleLPF;

// ── Compressor — nilai dari panel setelan ────────────────────────────────────
static audio_tools::Compressor   compressor(44100,
                                             COMP_ATTACK_MS,
                                             COMP_RELEASE_MS,
                                             50,                  // holdMs (tidak diekspos, nilai wajar)
                                             COMP_THRESHOLD_PCT,
                                             COMP_RATIO);

static SoftClipEffect            softClip;   // safety net — threshold dari CLIP_THRESHOLD

// Boost — nilai dari panel setelan: OUT_GAIN
static audio_tools::Boost        outGain(OUT_GAIN);

static DitherEffect              dither;

// Ring buffer: bridge antara fxStream output dan dac_write()
// 8192 byte = ~46ms @ 44100 Hz stereo 16-bit. Cukup besar untuk menghindari
// underrun tapi tidak menambah latency terasa.
static audio_tools::RingBufferStream g_ring(8192);

// fxStream: pointer untuk kontrol inisialisasi di setup()
static audio_tools::AudioEffectStreamT<int16_t> *fxStream = nullptr;

// Startup mute counter
static volatile uint32_t mute_remain = MUTE_FRAMES;

BluetoothA2DPSink a2dp_sink;


// ─────────────────────────────────────────────────────────────────────────────
// install_dac_i2s()
// ─────────────────────────────────────────────────────────────────────────────
static void install_dac_i2s()
{
    const i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX
                                             | I2S_MODE_DAC_BUILT_IN),
        .sample_rate          = 44100,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN));
    Serial.println("[I2S] DAC ready: GPIO25=L, GPIO26=R");
}


// ─────────────────────────────────────────────────────────────────────────────
// Static buffers — TIDAK boleh VLA di BT task (stack ~4–8 KB, mudah overflow).
// A2DP callback maksimal 4096 byte per call → pakai 4096 sebagai ukuran aman.
// ─────────────────────────────────────────────────────────────────────────────
static const size_t  BUF_SIZE = 4096;
static uint8_t       s_dsp_out[BUF_SIZE];    // output dari fxStream (DSP result)
static uint16_t      s_dac_buf[BUF_SIZE / 2]; // buffer konversi DAC (uint16, sama jumlah sample)


// ─────────────────────────────────────────────────────────────────────────────
// dac_write() — konversi int16 stereo signed → format DAC internal ESP32
//
//  ESP32 DAC (I2S_MODE_DAC_BUILT_IN):
//    • Hanya byte ATAS (bit 15..8) tiap word 16-bit yang dipakai sebagai DAC value
//    • Data harus UNSIGNED: dac = (int16 >> 8) + 128  (midpoint = 0x80)
//    • Hardware menukar L dan R → pre-swap di software
//
//  src       : pointer ke int16_t stereo interleaved [L0,R0,L1,R1,...]
//  num_frames: jumlah stereo frame (bukan jumlah sample total)
// ─────────────────────────────────────────────────────────────────────────────
static void dac_write(const int16_t *src, size_t num_frames)
{
    // s_dac_buf cukup untuk BUF_SIZE/2 uint16 = BUF_SIZE/4 stereo frame
    // Caller memastikan num_frames <= BUF_SIZE/4
    for (size_t i = 0; i < num_frames; i++) {
        // Konversi signed int16 → unsigned 8-bit tanpa offset bug:
        // +32767 → 255, 0 → 128, -32768 → 0
        // Shift arithmetic benar: (int16 + 32768) >> 8 = unsigned tanpa masalah sign
        uint8_t r = (uint8_t)(((int32_t)src[i * 2 + 1] + 32768) >> 8);  // Right
        uint8_t l = (uint8_t)(((int32_t)src[i * 2]     + 32768) >> 8);  // Left
        // Pre-swap L↔R: hardware DAC internal ESP32 menukar channel
        s_dac_buf[i * 2]     = (uint16_t)(r << 8);
        s_dac_buf[i * 2 + 1] = (uint16_t)(l << 8);
    }
    size_t written = 0;
    i2s_write(I2S_NUM, s_dac_buf, num_frames * 4, &written, portMAX_DELAY);
}


// ─────────────────────────────────────────────────────────────────────────────
// dac_silence() — kirim silence (0x80 midpoint) ke DAC sejumlah num_frames
// ─────────────────────────────────────────────────────────────────────────────
static void dac_silence(size_t num_frames)
{
    size_t words = num_frames * 2;
    if (words > BUF_SIZE / 2) words = BUF_SIZE / 2;
    for (size_t i = 0; i < words; i++) s_dac_buf[i] = 0x8000u;
    size_t written = 0;
    i2s_write(I2S_NUM, s_dac_buf, words * 2, &written, portMAX_DELAY);
}


// ─────────────────────────────────────────────────────────────────────────────
// process_dsp() — A2DP stream reader callback
//
//  PENTING: callback ini berjalan di FreeRTOS BT task dengan stack terbatas.
//  JANGAN alokasi buffer besar di stack (VLA, array lokal besar).
//  Semua buffer besar pakai static (s_dsp_out, s_dac_buf) yang di-share
//  — aman karena callback ini bersifat single-threaded (satu call selesai
//  sebelum call berikutnya dari task yang sama).
//
//  Alur:
//    1. Kirim data ke fxStream (DSP chain)
//    2. Drain output DSP dari g_ring ke s_dsp_out
//    3. Jika masih dalam startup mute: kirim silence ke DAC, buang DSP output
//    4. Normal: konversi dan kirim ke DAC via dac_write()
// ─────────────────────────────────────────────────────────────────────────────
void process_dsp(const uint8_t *data, uint32_t length)
{
    if (!fxStream) return;

    // Clamp length ke ukuran static buffer (seharusnya tidak pernah terlampaui)
    if (length > BUF_SIZE) length = BUF_SIZE;

    uint32_t num_frames = length / 4u;   // 2ch × 2 bytes = 4 bytes per stereo frame

    // ── Jalankan DSP chain ────────────────────────────────────────────────
    // Selalu proses audio melalui DSP agar state filter (DC blocker, gate,
    // LPF) tetap running selama mute — sehingga tidak ada transisi kasar
    // saat mute selesai.
    fxStream->write(data, (size_t)length);

    // Drain output dari ring ke s_dsp_out
    int avail = g_ring.available();
    if (avail <= 0) return;
    int to_read = avail < (int)length ? avail : (int)length;
    int got = (int)g_ring.readBytes(s_dsp_out, (size_t)to_read);
    if (got <= 0) return;

    size_t frames_got = (size_t)got / 4u;

    // ── Startup mute ─────────────────────────────────────────────────────
    // DSP sudah jalan tapi output ke DAC diganti silence.
    // Sederhana: tidak perlu split buffer, cukup ganti output.
    if (mute_remain > 0) {
        if (num_frames >= mute_remain) {
            mute_remain = 0;
            // Mute selesai di buffer ini — langsung buka suara
            // (transisi halus karena DSP sudah settling)
        } else {
            mute_remain -= num_frames;
            dac_silence(frames_got);
            return;
        }
    }

    // ── Kirim ke DAC ─────────────────────────────────────────────────────
    dac_write((const int16_t *)s_dsp_out, frames_got);
}


// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BT Speaker] Init...");

    // ── I2S DAC ──────────────────────────────────────────────────────────
    install_dac_i2s();

    // ── DSP pipeline ─────────────────────────────────────────────────────
    fxStream = new audio_tools::AudioEffectStreamT<int16_t>();
    fxStream->setOutput(g_ring);

    audio_tools::AudioInfo info;
    info.sample_rate     = 44100;
    info.bits_per_sample = 16;
    info.channels        = 2;
    fxStream->begin(info);

    fxStream->addEffect(dcBlock);      // [1] DC Blocker
    fxStream->addEffect(noiseGate);    // [2] Noise Gate
    fxStream->addEffect(twoPoleLPF);   // [3] LPF 14kHz
    fxStream->addEffect(compressor);   // [4] Compressor — jaga level saat keras
    fxStream->addEffect(softClip);     // [5] Soft Clip — safety net
    fxStream->addEffect(outGain);      // [6] Boost ×2.0
    fxStream->addEffect(dither);       // [7] TPDF Dither

    Serial.printf("[DSP] %d effects, %d channels\n",
                  (int)fxStream->size(), fxStream->channelCount());

    // ── A2DP ──────────────────────────────────────────────────────────────
    a2dp_sink.set_stream_reader(process_dsp, false);
    a2dp_sink.start(BT_DEVICE_NAME);
    Serial.println("[BT] Discoverable: ESP32-BT-Speaker");
    Serial.printf("[BT] Startup mute: %.0f ms\n",
                  MUTE_FRAMES / 44.1f);
}


// ─────────────────────────────────────────────────────────────────────────────
// loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop()
{
    static unsigned long t = 0;
    if (millis() - t >= 5000UL) {
        t = millis();
        const char *s = "?";
        switch (a2dp_sink.get_connection_state()) {
            case ESP_A2D_CONNECTION_STATE_DISCONNECTED:  s = "Disconnected";  break;
            case ESP_A2D_CONNECTION_STATE_CONNECTING:    s = "Connecting";    break;
            case ESP_A2D_CONNECTION_STATE_CONNECTED:     s = "Connected";     break;
            case ESP_A2D_CONNECTION_STATE_DISCONNECTING: s = "Disconnecting"; break;
            default: break;
        }
        Serial.printf("[%lus] %s | ring:%dB | mute:%lu\n",
                      millis()/1000UL, s, g_ring.available(),
                      (unsigned long)mute_remain);
    }
    delay(10);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * SEMUA SETELAN ADA DI PANEL ATAS (baris #define setelah #include).
 * Tidak perlu edit di sini.
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * REFERENSI CEPAT:
 *
 *   Suara terlalu PELAN   → Naikkan OUT_GAIN (misal: 2.5)
 *   Suara KRECEK/distorsi → Turunkan OUT_GAIN atau naikkan COMP_THRESHOLD_PCT
 *   Terlalu banyak HISS   → Naikkan GATE_THRESHOLD (misal: 150)
 *   Gate PUTUS-PUTUS      → Naikkan GATE_RELEASE_MS (misal: 200)
 *   Suara SEMBER/tajam    → Turunkan LPF_FC_HZ (misal: 10000)
 *   Suara terlalu HANGAT  → Naikkan LPF_FC_HZ (misal: 16000)
 *   Volume tidak STABIL   → Turunkan COMP_THRESHOLD_PCT atau COMP_RATIO
 *   Drum/bass tidak PUNCH → Naikkan COMP_ATTACK_MS (misal: 30)
 *   Terasa PUMPING        → Naikkan COMP_RELEASE_MS (misal: 300)
 *   Ada DENGUNG startup   → Naikkan MUTE_MS (misal: 1200)
 */
