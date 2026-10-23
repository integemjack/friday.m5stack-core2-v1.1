// ui.h — 屏幕与触摸
#pragma once
#include "app.h"

void ui_init(void);
void ui_draw(const char *qr_payload);   // qr_payload 非空时画配对二维码页（全屏白底大码）

// bot 选择页：列出名字（最多 6 个），等用户点；ui_pick_result 返回下标（未选=-1）
void ui_pick_bot(char (*names)[48], int n);
int ui_pick_result(void);
void ui_pick_reset(void);   // 选完后复位，恢复主界面刷新
void ui_tick(void);                     // 主循环里调：触摸采样 + 屏幕刷新（脏了才刷）

// ui 触发的动作回调（app_main 设）
typedef struct {
  void (*on_hold)(bool down);      // 按住说话（长条区）
  void (*on_interrupt)(void);      // 「停」
  void (*on_volume)(int delta);    // 音量键
} ui_callbacks_t;
void ui_set_callbacks(const ui_callbacks_t *cb);
