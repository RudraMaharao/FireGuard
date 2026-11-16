#include "storage.h"
#include "config.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "img_converters.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <atomic>

namespace {
const char *TAG = "storage";
const char *MOUNT = "/data";
const char *DIR_PATH = "/data/FIELD";  // 8.3 names everywhere, so any FAT tool can read them
const char *s_where = "NONE";
bool s_ready = false;
std::atomic<int> s_count{0};
std::recursive_mutex storage_mutex;

bool mount_sd()
{
    if (!USE_SD_CARD) {
        return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)SD_PIN_CLK;
    slot.cmd = (gpio_num_t)SD_PIN_CMD;
    slot.d0 = (gpio_num_t)SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;  // never wipe a card that has other files on it
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no SD card (%s), using internal flash", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool mount_flash()
{
    static wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;  // first boot: create the file system
    cfg.max_files = 4;
    cfg.allocation_unit_size = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, "storage", &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash storage not available: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

int highest_saved_number()
{
    int best = 0;
    DIR *d = opendir(DIR_PATH);
    if (!d) {
        return 0;
    }
    while (struct dirent *e = readdir(d)) {
        int n = 0;
        if ((e->d_name[0] == 'F' || e->d_name[0] == 'f') && sscanf(e->d_name + 1, "%d", &n) == 1 && n > best) {
            best = n;
        }
    }
    closedir(d);
    return best;
}
}  // namespace

namespace storage {
void lock(){storage_mutex.lock();}
void unlock(){storage_mutex.unlock();}

bool init()
{
    if (!SAVE_FEEDBACK) {
        return false;
    }
    if (mount_sd()) {
        s_where = "SD";
    } else if (mount_flash()) {
        s_where = "FLASH";
    } else {
        return false;
    }
    mkdir(DIR_PATH, 0775);
    s_count = highest_saved_number();
    s_ready = true;
    ESP_LOGI(TAG, "saving feedback to %s, %d samples so far", s_where, int(s_count));
    return true;
}

bool ready() { return s_ready; }
const char *dir() { return DIR_PATH; }

int clear()
{
    Guard guard;
    int removed = 0;
    DIR *d = opendir(DIR_PATH);
    if (!d) {
        return 0;
    }
    char path[300];
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", DIR_PATH, e->d_name);
        if (unlink(path) == 0) {
            removed++;
        }
    }
    closedir(d);
    s_count = 0;
    ESP_LOGI(TAG, "cleared %d files", removed);
    return removed;
}
const char *where() { return s_where; }
int count() { return s_count; }

const char *feedback_name(Feedback fb)
{
    switch (fb) {
    case CORRECT: return "correct";
    case FALSE_ALARM: return "false_alarm";
    case MISSED_FIRE: return "missed_fire";
    case MISSED_SMOKE: return "missed_smoke";
    }
    return "?";
}

int save(const uint8_t *img, int w, int h, const Verdict &v, Feedback fb, int took_ms)
{
    Guard guard;
    if (!s_ready) {
        return -1;
    }
    const int id = s_count + 1;
    char path[48];

    // 1) the photo as JPEG
    uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    if (!fmt2jpg((uint8_t *)img, w * h * 2, w, h, PIXFORMAT_RGB565, JPEG_QUALITY, &jpg, &jpg_len)) {
        ESP_LOGE(TAG, "JPEG encoding failed");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/F%04d.JPG", DIR_PATH, id);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(jpg, 1, jpg_len, f) == jpg_len;
    if (f) {
        fclose(f);
    }
    free(jpg);
    if (!ok) {
        ESP_LOGE(TAG, "could not write %s (storage full?)", path);
        return -1;
    }

    // 2) the model's boxes in YOLO format (class x_center y_center width height, 0..1)
    snprintf(path, sizeof(path), "%s/F%04d.TXT", DIR_PATH, id);
    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < v.count; i++) {
            const Detection &d = v.det[i];
            fprintf(f, "%d %.5f %.5f %.5f %.5f %.3f\n", d.cls, (d.x1 + d.x2) / 2.0f / w, (d.y1 + d.y2) / 2.0f / h,
                    (float)(d.x2 - d.x1) / w, (float)(d.y2 - d.y1) / h, d.score);
        }
        fclose(f);
    }

    // 3) one line in the log with your verdict
    snprintf(path, sizeof(path), "%s/LOG.CSV", DIR_PATH);
    struct stat st;
    bool new_file = stat(path, &st) != 0;
    f = fopen(path, "a");
    if (f) {
        if (new_file) {
            fprintf(f, "id,image,feedback,model_fire,model_smoke,boxes,took_ms\n");
        }
        fprintf(f, "%d,F%04d.JPG,%s,%.3f,%.3f,%d,%d\n", id, id, feedback_name(fb), v.best_fire, v.best_smoke, v.count,
                took_ms);
        fclose(f);
    }
    s_count = id;
    ESP_LOGI(TAG, "saved sample %d (%s) to %s", id, feedback_name(fb), s_where);
    return id;
}

}  // namespace storage
