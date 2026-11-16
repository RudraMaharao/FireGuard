// Saves field samples (photo + model answer + your verdict) to microSD or internal flash.
#pragma once
#include "detection_types.h"
#include <stdint.h>

namespace storage {
void lock();
void unlock();
struct Guard { Guard(){lock();} ~Guard(){unlock();} };
enum Feedback { CORRECT, FALSE_ALARM, MISSED_FIRE, MISSED_SMOKE };

bool init();                 // true if somewhere to save was found
bool ready();
const char *where();         // "SD" or "FLASH" (or "NONE")
int count();                 // samples saved so far
// Saves one sample. Returns its number (1, 2, ...) or -1 on failure.
int save(const uint8_t *rgb565be, int w, int h, const Verdict &v, Feedback fb, int took_ms);
const char *feedback_name(Feedback fb);
const char *dir();           // folder with the samples, e.g. "/data/FIELD"
int clear();                 // deletes all samples, returns how many files were removed
}  // namespace storage
