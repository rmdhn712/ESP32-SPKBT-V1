/**
 * AudioConfigLocal.h — Override konfigurasi audio-tools untuk project ini.
 * 
 * File ini di-include otomatis oleh AudioToolsConfig.h jika ada di sketch folder.
 * Letakkan di folder yang sama dengan BT_Speaker.ino.
 *
 * Referensi: AudioToolsConfig.h baris "#if __has_include("AudioConfigLocal.h")"
 */

// WAJIB: Nonaktifkan "using namespace audio_tools" otomatis.
// Tanpa ini, audio-tools menambahkan "using namespace audio_tools" secara global,
// yang menyebabkan konflik simbol dengan ESP32-A2DP (misalnya AudioInfo, Stream, dll.)
#define USE_AUDIOTOOLS_NS false
