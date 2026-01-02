#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/i2s_std.h"
#include "sdmmc_cmd.h"

// Используем заголовок из ваших логов (dr_mp3)
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h" 

// === Настройки Пинов ===
#define PIN_SD_SCLK    18
#define PIN_SD_MOSI    23
#define PIN_SD_MISO    19
#define PIN_SD_CS      14

#define I2S_BCLK_PIN   26
#define I2S_LRCK_PIN   25  
#define I2S_DOUT_PIN   17

#define NEXT_BUTTON_PIN 27
static const char *TAG = "MP3_PLAYER";

// === Глобальные переменные для экономии стека ===
i2s_chan_handle_t tx_handle;
static drmp3dec mp3d;
static uint8_t input_buf[8192];
static int16_t pcm_buf[DRMP3_MAX_SAMPLES_PER_FRAME];


TaskHandle_t player_task_handle = NULL;
volatile bool skip_current_track = false;

// Обработчик прерывания кнопки
static void IRAM_ATTR button_isr_handler(void* arg) {
    skip_current_track = true; 
}

void init_button() {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << NEXT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1, // СТРОГО 1
        .pull_down_en = 0,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(NEXT_BUTTON_PIN, button_isr_handler, NULL);
}

void init_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Увеличиваем буферы до максимума для компенсации SPI
    chan_cfg.dma_desc_num = 6;      // Было 6
    chan_cfg.dma_frame_num = 512;   // Было 256
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_DOUT_PIN,
        },
    };
    i2s_channel_init_std_mode(tx_handle, &std_cfg);
}

// Функция для применения затухания к буферу PCM (16-бит стерео/моно)
void apply_fade_out(int16_t *data, int samples, int channels, float *volume) {
    for (int i = 0; i < samples * channels; i++) {
        data[i] = (int16_t)(data[i] * (*volume));
        *volume -= (1.0f / (samples * channels * 10.0f)); // Скорость затухания
        if (*volume < 0) *volume = 0;
    }
}



// Измененная функция проигрывания
void play_file(const char* path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;

    skip_current_track = false; // Сбрасываем флаг перед началом
    drmp3dec_init(&mp3d);
    drmp3dec_frame_info info;
    i2s_channel_enable(tx_handle);
    ESP_LOGI(TAG, "Playing: %s", path);
    size_t n_read = fread(input_buf, 1, sizeof(input_buf), f);
    uint8_t* read_ptr = input_buf;

    while (n_read > 0) {
        int samples = drmp3dec_decode_frame(&mp3d, read_ptr, n_read, pcm_buf, &info);
        // ПРОВЕРКА КНОПКИ: если нажата, выходим из цикла
        if (skip_current_track) {
            ESP_LOGW(TAG, "Skipping track...");
            break;
        }
        if (samples > 0) {
            size_t bw;
            i2s_channel_write(tx_handle, pcm_buf, samples * info.channels * sizeof(int16_t), &bw, portMAX_DELAY);
        }
        
        if (info.frame_bytes == 0) break;
        n_read -= info.frame_bytes;
        read_ptr += info.frame_bytes;

        if (n_read < 1024) { 
            memmove(input_buf, read_ptr, n_read);
            size_t loaded = fread(input_buf + n_read, 1, sizeof(input_buf) - n_read, f);
            n_read += loaded;
            read_ptr = input_buf;
            if (loaded == 0 && n_read == 0) break;
        }
    }


    fclose(f);
    i2s_channel_disable(tx_handle);   
    // Небольшая задержка для защиты от дребезга кнопки при переключении
    if (skip_current_track) {
        vTaskDelay(pdMS_TO_TICKS(600));
        skip_current_track = false;
    }
}

void recursive_play(const char *base_path) {
    struct dirent *entry;
    DIR *dir = opendir(base_path);

    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir: %s", base_path);
        return;
    }

    // Увеличиваем до 300, чтобы точно влез путь FAT (256) + имя папки
    char *path = malloc(300);
    if (!path) {
        ESP_LOGE(TAG, "Memory allocation failed for path");
        closedir(dir);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Используем snprintf и проверяем результат, чтобы компилятор не ругался
        int len = snprintf(path, 300, "%s/%s", base_path, entry->d_name);
        if (len >= 300) {
            ESP_LOGW(TAG, "Path too long, skipping: %s", entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                recursive_play(path); // Рекурсия
            } else {
                char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcasecmp(ext, ".mp3") == 0)) {
                    play_file(path);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
        }
    }

    free(path); // Освобождаем память перед выходом из текущего уровня рекурсии
    closedir(dir);
}

void music_player_task(void *pvParameters) {
    while(1) {
        ESP_LOGI(TAG, "Starting full SD scan and play...");
        recursive_play("/sdcard");
        ESP_LOGI(TAG, "All files played. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    esp_err_t ret;
    
    // 1. Инициализация I2S
    init_i2s();

    // 2. Ваша рабочая инициализация SD
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST;
    host.max_freq_khz=5000;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2, // Уменьшено для экономии RAM
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed");
        return;
    }
    init_button();
    // 3. Запуск проигрывания в отдельной задаче (стек 10 КБ)
    xTaskCreate(music_player_task, "player", 10240, NULL, 15, NULL);
    
}
