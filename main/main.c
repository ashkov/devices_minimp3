#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

// Используем заголовок dr_mp3 (minimp3)
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

static const char *TAG = "MP3_PLAYER";

// === Конфигурация пинов ===
#define PIN_SD_SCLK    18
#define PIN_SD_MOSI    23
#define PIN_SD_MISO    19
#define PIN_SD_CS      14
#define I2S_BCLK_PIN   26
#define I2S_LRCK_PIN   25
#define I2S_DOUT_PIN   17
#define BTN_NEXT_PIN   27
#define BTN_PREV_PIN   16

// === Глобальные переменные ===
i2s_chan_handle_t tx_handle;
static drmp3dec mp3d;
static uint8_t input_buf[8192];
static int16_t pcm_buf[DRMP3_MAX_SAMPLES_PER_FRAME];

typedef enum { PLAY_FINISHED, PLAY_NEXT, PLAY_PREV } play_status_t;

char current_full_path[256] = "";
char found_path[256] = "";
char last_seen_path[256] = "";
volatile bool skip_requested = false;
volatile bool prev_requested = false;
bool play_previous = false;
bool target_found = false;

// === Обработка кнопок ===
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    if (gpio_num == BTN_NEXT_PIN) skip_requested = true;
    if (gpio_num == BTN_PREV_PIN) prev_requested = true;
}

void init_peripherals() {
    // 1. Инициализация I2S (Philips Standard)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.bclk = I2S_BCLK_PIN, .ws = I2S_LRCK_PIN, .dout = I2S_DOUT_PIN}
    };
    i2s_channel_init_std_mode(tx_handle, &std_cfg);

    // 2. Инициализация кнопок
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << BTN_NEXT_PIN) | (1ULL << BTN_PREV_PIN),
        .mode = GPIO_MODE_INPUT, .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_NEXT_PIN, button_isr_handler, (void*)BTN_NEXT_PIN);
    gpio_isr_handler_add(BTN_PREV_PIN, button_isr_handler, (void*)BTN_PREV_PIN);
}

void init_sd_card() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST;
    host.max_freq_khz = 5000; // Стабильная скорость для исключения шумов

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI, .miso_io_num = PIN_SD_MISO, .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096
    };
    spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {.format_if_mount_failed = false, .max_files = 2};
    sdmmc_card_t *card;
    esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_cfg, &card);
}

// === Поиск следующего/предыдущего файла по всей карте ===
void scan_for_neighbor(const char *base_path) {
    if (target_found) return;
    DIR *dir = opendir(base_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (target_found) break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_for_neighbor(path);
            } else {
                char *ext = strrchr(entry->d_name, '.');
                if (ext && strcasecmp(ext, ".mp3") == 0) {
                    if (!play_previous) { // Поиск следующего
                        if (strlen(current_full_path) == 0) { strcpy(found_path, path); target_found = true; break; }
                        if (strcmp(path, current_full_path) == 0) { found_path[0] = '\0'; } // Маркер найденного текущего
                        else if (found_path[0] == '\0') { strcpy(found_path, path); target_found = true; break; }
                    } else { // Поиск предыдущего
                        if (strcmp(path, current_full_path) == 0) {
                            if (strlen(last_seen_path) > 0) { strcpy(found_path, last_seen_path); target_found = true; break; }
                        }
                        strcpy(last_seen_path, path);
                    }
                }
            }
        }
    }
    closedir(dir);
}

// === Функция воспроизведения ===
play_status_t play_file(const char* path) {
    FILE *f = fopen(path, "rb");
    if (!f) return PLAY_FINISHED;

    skip_requested = false; prev_requested = false;
    drmp3dec_init(&mp3d);
    drmp3dec_frame_info info;

    // Очистка I2S перед началом (убирает хвосты)
    i2s_channel_enable(tx_handle);
    memset(pcm_buf, 0, sizeof(pcm_buf));
    size_t bw;
    for(int i=0; i<8; i++) i2s_channel_write(tx_handle, pcm_buf, sizeof(pcm_buf), &bw, portMAX_DELAY);

    ESP_LOGI(TAG, "File: %s", path);

    size_t n_read = fread(input_buf, 1, sizeof(input_buf), f);
    uint8_t* read_ptr = input_buf;

    uint32_t current_sample_rate = 0;

    while (n_read > 0) {
        if (skip_requested) { fclose(f); i2s_channel_disable(tx_handle); return PLAY_NEXT; }
        if (prev_requested) { fclose(f); i2s_channel_disable(tx_handle); return PLAY_PREV; }

        int samples = drmp3dec_decode_frame(&mp3d, read_ptr, n_read, pcm_buf, &info);

        if (samples > 0) {
            // Если частота в файле изменилась или это начало файла
            if (info.sample_rate != current_sample_rate) {
                i2s_channel_disable(tx_handle);
                current_sample_rate = info.sample_rate;

                ESP_LOGI(TAG, "Changing I2S rate to: %lu, Channels: %d", info.sample_rate, info.channels);
                
                // В v5.x перенастройка делается через i2s_channel_reconfig_std_clock
                i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_sample_rate);
                i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
                i2s_channel_enable(tx_handle);
            }

            size_t bw;
            if (info.channels == 1) {
                // Если файл моно, расширяем его до стерео "на лету" прямо в том же буфере
                // Идем с конца буфера к началу, чтобы не затереть данные
                for (int i = samples - 1; i >= 0; i--) {
                    pcm_buf[i * 2]     = pcm_buf[i]; // Левый канал
                    pcm_buf[i * 2 + 1] = pcm_buf[i]; // Правый канал
                }
                // Теперь данных в 2 раза больше, и они выглядят как стерео
                i2s_channel_write(tx_handle, pcm_buf, samples * 2 * sizeof(int16_t), &bw, portMAX_DELAY);
            } else {
                // Если файл уже стерео, пишем как обычно
                i2s_channel_write(tx_handle, pcm_buf, samples * info.channels * sizeof(int16_t), &bw, portMAX_DELAY);
            }
        }

        if (info.frame_bytes == 0) break;
        n_read -= info.frame_bytes; read_ptr += info.frame_bytes;

        if (n_read < 1024) {
            memmove(input_buf, read_ptr, n_read);
            n_read += fread(input_buf + n_read, 1, sizeof(input_buf) - n_read, f);
            read_ptr = input_buf;
        }
    }

    fclose(f);
    i2s_channel_disable(tx_handle);
    return PLAY_FINISHED;
}

void music_player_task(void *pvParameters) {
    while(1) {
        target_found = false; found_path[0] = ' '; last_seen_path[0] = '\0';
        scan_for_neighbor("/sdcard");

        if (!target_found) { // Если конец списка или ошибка
            current_full_path[0] = '\0'; play_previous = false;
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }

        strcpy(current_full_path, found_path);
        play_status_t res = play_file(current_full_path);

        play_previous = (res == PLAY_PREV);
        vTaskDelay(pdMS_TO_TICKS(300)); // Пауза между треками
    }
}

void app_main(void) {
    init_peripherals();
    init_sd_card();
    xTaskCreatePinnedToCore(music_player_task, "player", 10240, NULL, 5, NULL, 1);
}
