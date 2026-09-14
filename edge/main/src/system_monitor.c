#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

uint8_t get_cpu_usage_percentage(void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t start_array[16], end_array[16];
    UBaseType_t start_count, end_count;
    uint32_t start_run_time, end_run_time;

    // Ambil snapshot snapshot waktu 1
    start_count = uxTaskGetSystemState(start_array, 16, &start_run_time);
    vTaskDelay(pdMS_TO_TICKS(100)); // Sampel selama 100ms
    // Ambil snapshot waktu 2
    end_count = uxTaskGetSystemState(end_array, 16, &end_run_time);

    uint32_t total_elapsed = end_run_time - start_run_time;
    if (total_elapsed == 0) return 0;

    uint32_t idle_time = 0;
    for (UBaseType_t i = 0; i < end_count; i++) {
        // Cari IDLE task (IDLE0 & IDLE1 jika Dual Core)
        if (strncmp(end_array[i].pcTaskName, "IDLE", 4) == 0) {
            for (UBaseType_t j = 0; j < start_count; j++) {
                if (end_array[i].xHandle == start_array[j].xHandle) {
                    idle_time += (end_array[i].ulRunTimeCounter - start_array[j].ulRunTimeCounter);
                    break;
                }
            }
        }
    }

    // Hitung persentase CPU terpakai
    // Note: Untuk Dual-Core (ESP32-S3), total time dikali jumlah core
    uint32_t cpu_busy = total_elapsed - (idle_time / CONFIG_FREERTOS_NUMBER_OF_CORES);
    uint32_t cpu_percentage = (cpu_busy * 100) / total_elapsed;

    if (cpu_percentage > 100) cpu_percentage = 100;
    return (uint8_t)cpu_percentage;
#else
    return 0; // Kembalikan 0 jika flag statistik tidak diaktifkan
#endif
}