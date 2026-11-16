#pragma once
namespace buzzer {
void init();
void set(bool on);
void beep(int ms);  // blocking
}
