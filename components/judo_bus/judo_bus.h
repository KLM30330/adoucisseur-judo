#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"

#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace judo_bus {

#define JUDO_SENSOR(name) \
 public: \
  void set_##name##_sensor(sensor::Sensor *s) { this->name##_ = s; } \
 protected: \
  sensor::Sensor *name##_{nullptr};

#define JUDO_BINARY(name) \
 public: \
  void set_##name##_binary_sensor(binary_sensor::BinarySensor *s) { this->name##_ = s; } \
 protected: \
  binary_sensor::BinarySensor *name##_{nullptr};

#define JUDO_TEXT(name) \
 public: \
  void set_##name##_text_sensor(text_sensor::TextSensor *s) { this->name##_ = s; } \
 protected: \
  text_sensor::TextSensor *name##_{nullptr};

class JudoBus : public Component, public uart::UARTDevice {
 public:
  void set_address(uint8_t a) { this->address_ = a; }
  void set_device_type(uint8_t t) { this->device_type_ = t; }
  void set_bus_timeout(uint32_t ms) { this->bus_timeout_ms_ = ms; }
  void set_regeneration_hold(uint32_t ms) { this->regen_hold_ms_ = ms; }
  void set_maintenance_warning_days(int d) { this->maint_warn_days_ = d; }
  void set_log_changes(bool b) { this->log_changes_ = b; }
  void set_master(bool b) { this->master_ = b; }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_session_init(bool b) { this->session_init_ = b; }
  void set_capacity_threshold(uint16_t t) { this->capacity_threshold_ = t; }
  void reset_salt_alarm();
  void set_tx_uart(uart::UARTComponent *u) { this->tx_uart_ = u; }
  void set_log_raw_frames(bool b) { this->log_raw_ = b; }
  void set_power_switch(switch_::Switch *s) { this->power_switch_ = s; }
  void set_power_switch_cuts(bool b) { this->power_switch_cuts_ = b; }
  void set_power_cycle_on_boot(bool b) { this->boot_cycle_pending_ = b; }
  void set_stuck_timeouts(uint16_t n) { this->stuck_threshold_ = n; }
  void power_cycle_module(const char *reason);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  JUDO_SENSOR(soft_water_total)
  JUDO_SENSOR(flow_rate)
  JUDO_SENSOR(regeneration_count)
  JUDO_SENSOR(raw_hardness)
  JUDO_SENSOR(residual_hardness)
  JUDO_SENSOR(maintenance_days)
  JUDO_SENSOR(operating_days)
  JUDO_SENSOR(device_number)
  JUDO_SENSOR(status_countdown)
  JUDO_SENSOR(capacity_counter)
  JUDO_SENSOR(flow_indicator)
  JUDO_SENSOR(regeneration_remaining)
  JUDO_SENSOR(water_today)
  JUDO_SENSOR(flow_instant)
  JUDO_SENSOR(capacity_remaining)
  JUDO_SENSOR(capacity_at_regeneration)

  JUDO_BINARY(salt_shortage)
  JUDO_BINARY(maintenance_due)
  JUDO_BINARY(regeneration)
  JUDO_BINARY(bus_ok)

  JUDO_TEXT(hardware_version)
  JUDO_TEXT(software_version)
  JUDO_TEXT(hardness_unit)
  JUDO_TEXT(message_history)
  JUDO_TEXT(last_event)
  JUDO_TEXT(unknown_change)
  JUDO_TEXT(bus_diagnostic)

 protected:
  void parse_buffer_();
  void handle_frame_(uint16_t cmd, const uint8_t *d, uint16_t len);
  void handle_status_block_(const uint8_t *d, uint16_t len);
  void track_changes_(uint32_t key, const char *label, const uint8_t *d, uint16_t len,
                      const std::vector<uint8_t> &volatile_idx);
  void event_(const std::string &msg);
  static uint16_t crc16_(const uint8_t *d, size_t n);
  void set_salt_latched_(bool on);

  // régénération (bit 0) et alerte sel mémorisée
  bool regen_bit_known_{false};
  bool regen_bit_{false};
  bool regen_salt_drop_{false};
  bool salt_latched_{false};
  bool salt_published_{false};
  ESPPreferenceObject salt_pref_;
  // capacité
  uint16_t capacity_threshold_{3000};
  bool capacity_known_{false};
  uint16_t last_capacity_{0};

  // --- Mode maître (remplace le module Connectivity) ---
  struct PollItem {
    uint16_t cmd;
    int16_t page;       // -1 = pas de donnée, sinon 1 octet (commande 0x0043)
    uint32_t period_ms;
    uint32_t next_ms;
  };
  void master_loop_();
  void send_request_(uint16_t cmd, const uint8_t *data, uint16_t len);
  bool master_{false};
  bool log_raw_{false};
  // Alimentation du module RS485 pilotée (redémarrage automatique s'il se bloque)
  void power_loop_();
  switch_::Switch *power_switch_{nullptr};
  bool power_switch_cuts_{true};
  bool boot_cycle_pending_{false};
  bool power_cycling_{false};
  uint32_t power_restore_ms_{0};
  uint32_t last_power_cycle_ms_{0};
  bool power_cycled_once_{false};
  uint16_t stuck_threshold_{20};
  uint32_t power_cycles_{0};
  std::vector<uint8_t> raw_burst_;
  uint32_t raw_burst_start_ms_{0};
  uint32_t last_raw_ms_{0};
  uart::UARTComponent *tx_uart_{nullptr};  // UART d'émission séparé (ex. UART1 sur GPIO2)
  bool session_init_{false};
  bool session_done_{false};
  uint32_t poll_interval_ms_{500};
  uint32_t last_rx_byte_ms_{0};
  uint32_t last_tx_ms_{0};
  bool awaiting_{false};
  uint16_t awaiting_cmd_{0};
  uint32_t timeouts_{0};
  uint32_t consecutive_timeouts_{0};
  uint32_t master_paused_until_{0};
  std::vector<PollItem> poll_;

  uint8_t address_{0x01};
  uint8_t device_type_{0x38};
  uint32_t bus_timeout_ms_{300000};
  uint32_t regen_hold_ms_{5400000};
  int maint_warn_days_{0};
  bool log_changes_{true};

  std::vector<uint8_t> buf_;
  uint32_t last_valid_ms_{0};
  uint32_t frames_ok_{0};
  uint32_t crc_errors_{0};
  uint32_t rx_bytes_{0};
  uint32_t tx_frames_{0};
  uint32_t last_diag_ms_{0};
  uint32_t last_check_ms_{0};
  bool bus_state_known_{false};
  bool bus_state_{false};

  // compteur d'eau et débit calculé
  bool water_known_{false};
  uint32_t water_l_{0};
  uint32_t water_poll_ms_{0};
  uint32_t water_change_ms_{0};

  // régénération
  bool regen_known_{false};
  uint16_t regen_count_{0};
  uint32_t regen_start_ms_{0};
  bool regen_on_{false};

  // sel
  bool brine_known_{false};
  bool brine_ok_{true};

  // page demandée par le module pour la commande 0x0043
  int last_page_{-1};
  uint32_t last_page_ms_{0};

  std::map<uint32_t, std::vector<uint8_t>> last_payload_;
};

}  // namespace judo_bus
}  // namespace esphome
