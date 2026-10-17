#pragma once
#include <stdint.h>

// Unsigned subtraction keeps cooldowns correct across millis() rollover.
class PressGuard {
 public:
  bool ready(uint32_t now, uint32_t connectedAt, uint32_t connectionGuard,
             uint32_t cooldown) const {
    return uint32_t(now - connectedAt) >= connectionGuard &&
           (!hasPressed_ || uint32_t(now - lastPress_) >= cooldown);
  }
  void record(uint32_t now) { lastPress_ = now; hasPressed_ = true; }

 private:
  bool hasPressed_ = false;
  uint32_t lastPress_ = 0;
};
