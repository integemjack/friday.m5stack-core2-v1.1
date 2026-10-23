// audio_io.h — 麦克风 / 扬声器半双工管道（M5Unified）
//
// Core2：Mic = I2S0 PDM（CLK=GPIO0, DATA=GPIO34），Speaker = I2S1（WS=GPIO0）。
// 两者共享 GPIO0，M5Unified 的队列模型 + 本模块的半双工门（播放时不录）规避。
#pragma once
#include <stdint.h>
#include <stdbool.h>

// 初始化（M5.begin 之后调）；mic_task 循环读 Mic 队列 → work_feed_audio
void audio_start(void);
void audio_stop(void);
void audio_beep(int ms) __attribute__((unused));     // 调试用：直接放一段 440Hz 正弦

// work.cpp 收到 voice.audio 解码后调
void audio_play(const int16_t *pcm, int samples);
void audio_clear(void);          // voice.clearAudio / interrupt
void audio_duck(bool on);        // voice.duck：播报时压低外放
void audio_pause(bool paused);   // voice.pause
bool audio_playing(void);
void audio_set_volume(int v01);  // 0..255（M5 口径）
int  audio_volume(void);

// 按住说话：down=true 开麦克风（半双工：扬声器让出 GPIO0），false 关
void audio_hold(bool down);