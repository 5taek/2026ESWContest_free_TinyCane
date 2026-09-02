#include "common.h"

static const char *TAG_I2C = "I2C";

// ════════════════════════════════════════════════════════════
// 전역 핸들 정의
// ════════════════════════════════════════════════════════════
i2c_master_bus_handle_t bus_handle = NULL;
i2c_master_bus_handle_t bus_handle1 = NULL;
i2c_master_dev_handle_t tof_dev1   = NULL;
i2c_master_dev_handle_t tof_dev2   = NULL;
i2c_master_dev_handle_t DRV_dev1   = NULL;
i2c_master_dev_handle_t DRV_dev2   = NULL;

// ════════════════════════════════════════════════════════════
// I2C 마스터 버스 초기화
// ════════════════════════════════════════════════════════════
void I2C_master_INIT(void) {
    i2c_master_bus_config_t bus_config1 = {};
    bus_config1.i2c_port              = I2C_NUM_0;
    bus_config1.sda_io_num            = (gpio_num_t)I2C_SDA_PIN;
    bus_config1.scl_io_num            = (gpio_num_t)I2C_SCL_PIN;
    bus_config1.clk_source            = I2C_CLK_SRC_DEFAULT;
    bus_config1.glitch_ignore_cnt     = 7;
    bus_config1.intr_priority         = 0;
    bus_config1.trans_queue_depth     = 0;
    bus_config1.flags.enable_internal_pullup = 1;
    bus_config1.flags.allow_pd        = 0;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config1, &bus_handle));
    ESP_LOGI(TAG_I2C, "✓ I2C 마스터 버스 초기화 완료 (SDA=%d, SCL=%d)",
             I2C_SDA_PIN, I2C_SCL_PIN);

    i2c_master_bus_config_t bus_config2 = {};
    bus_config2.i2c_port              = I2C_NUM_1;
    bus_config2.sda_io_num            = (gpio_num_t)I2C1_SDA_PIN;
    bus_config2.scl_io_num            = (gpio_num_t)I2C1_SCL_PIN;
    bus_config2.clk_source            = I2C_CLK_SRC_DEFAULT;
    bus_config2.glitch_ignore_cnt     = 7;
    bus_config2.intr_priority         = 0;
    bus_config2.trans_queue_depth     = 0;
    bus_config2.flags.enable_internal_pullup = 1;
    bus_config2.flags.allow_pd        = 0;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config2, &bus_handle1));
    ESP_LOGI(TAG_I2C, "✓ I2C 마스터 버스 초기화 완료 (SDA=%d, SCL=%d)",
             I2C1_SDA_PIN, I2C1_SCL_PIN);
}

// ════════════════════════════════════════════════════════════
// I2C 버스 스캔 — 응답하는 모든 주소 출력 (디버깅 핵심!)
//
// ⚠️ 예전엔 0바이트 i2c_master_transmit()로 주소를 프로브했는데(디바이스를 매 주소마다
// add/remove하면서), 지금 쓰는 IDF v6.0의 i2c_master 드라이버는 size=0 전송 자체를
// 버스에 나가기도 전에 ESP_ERR_INVALID_ARG("i2c transmit buffer or size invalid")로
// 거부한다. 그래서 실제 버스 상태와 무관하게 항상 "0개 발견"만 나오는 상태였다
// (DRV가 같은 버스에서 실제로 정상 통신되는 걸로 확인됨 — 스캔 결과가 거짓이었음).
// i2c_master_probe()는 이 정확한 용도(주소 존재 확인)를 위해 IDF가 제공하는 API라
// 매 주소마다 add/remove할 필요도 없고 size=0 문제도 없다.
// ════════════════════════════════════════════════════════════
void I2C_SCAN(i2c_master_bus_handle_t bus, const char *tag) {
    ESP_LOGI(TAG_I2C, "━━━━━━ I2C SCAN: %s ━━━━━━", tag);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG_I2C, "    ✓ 0x%02X 응답!", addr);
            found++;
        }
    }
    ESP_LOGI(TAG_I2C, "━━━━━━ %s: 총 %d개 발견 ━━━━━━", tag, found);
}