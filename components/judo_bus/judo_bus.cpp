#include "judo_bus.h"
#include <cstring>
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <cinttypes>
#include <cstdio>

namespace esphome {
namespace judo_bus {

static const char *const TAG = "judo_bus";
static const size_t MAX_PAYLOAD = 256;
static const size_t MAX_BUF = 1024;

// Commandes identifiées (Quicksoft-M, type 0x38)
static const uint16_t CMD_SW_VERSION = 0x0001;     // 3 o : ?, mineur, majeur (64 05 01 = 1.05)
static const uint16_t CMD_HW_VERSION = 0x0002;     // 2 o : mineur, majeur (14 05 = 5.20)
static const uint16_t CMD_DEVICE_NUMBER = 0x0006;  // 4 o LE
static const uint16_t CMD_HARDNESS_UNIT = 0x0023;  // 1 o
static const uint16_t CMD_OPERATING_TIME = 0x0025; // min, h, jours(2 o)
static const uint16_t CMD_MAINTENANCE = 0x0026;    // heures restantes (2 o) + 4 o
static const uint16_t CMD_RAW_WATER = 0x0028;      // 4 o (0 sur Quicksoft)
static const uint16_t CMD_SOFT_WATER = 0x0029;     // litres, 4 o LE
static const uint16_t CMD_RAW_HARDNESS = 0x0052;   // 2 o
static const uint16_t CMD_RAW_HARDNESS2 = 0x0054;  // 2 o (même valeur observée)
static const uint16_t CMD_STATUS = 0x0043;         // blocs d'état 32 / 64 o

// Types de blocs 0x0043
enum Block : uint8_t { BLK_UNKNOWN = 0, BLK_A, BLK_B, BLK_C, BLK_HIST_DAYS, BLK_HIST_CODES, BLK_PAGE };

static std::string hex_(const uint8_t *d, size_t n) {
  static const char *hx = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    if (i)
      s += ' ';
    s += hx[d[i] >> 4];
    s += hx[d[i] & 0xF];
  }
  return s;
}

static uint16_t u16_(const uint8_t *d) { return (uint16_t) (d[0] | (d[1] << 8)); }
static uint32_t u32_(const uint8_t *d) {
  return (uint32_t) d[0] | ((uint32_t) d[1] << 8) | ((uint32_t) d[2] << 16) | ((uint32_t) d[3] << 24);
}

uint16_t JudoBus::crc16_(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t) d[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
  }
  return crc;
}

void JudoBus::setup() {
  this->buf_.reserve(MAX_BUF);
  if (this->master_) {
    // Plan d'interrogation inspiré du module Connectivity
    const uint32_t S = 1000;
    this->poll_ = {
        // Démarrage identique au module Connectivity : version matérielle puis logicielle
        {CMD_HW_VERSION, -1, 300 * S, 0},
        {CMD_SW_VERSION, -1, 300 * S, 0},
        {CMD_STATUS, 1, 2 * S, 0},          // bloc A : sel, régénérations, capacité
        {CMD_SOFT_WATER, -1, 5 * S, 0},     // compteur d'eau adoucie
        {CMD_STATUS, 0, 10 * S, 0},         // bloc B
        {CMD_STATUS, 2, 10 * S, 0},         // bloc C
        {CMD_OPERATING_TIME, -1, 60 * S, 0},
        {CMD_MAINTENANCE, -1, 60 * S, 0},
        {CMD_STATUS, 3, 300 * S, 0},        // historique (jours)
        {CMD_STATUS, 4, 300 * S, 0},        // historique (codes)
        {CMD_RAW_HARDNESS, -1, 300 * S, 0},
        {CMD_RAW_HARDNESS2, -1, 300 * S, 0},
        {CMD_HARDNESS_UNIT, -1, 300 * S, 0},
        {CMD_DEVICE_NUMBER, -1, 300 * S, 0},
        {CMD_RAW_WATER, -1, 300 * S, 0},
        {0x0055, -1, 300 * S, 0},
        {0x0056, -1, 300 * S, 0},
        {0x0021, -1, 300 * S, 0},
        {0x0033, -1, 300 * S, 0},
        {0x0018, -1, 300 * S, 0},
    };
    // Premier passage étalé après le démarrage
    uint32_t t = 5000;
    for (auto &p : this->poll_) {
      p.next_ms = t;
      t += this->poll_interval_ms_;
    }
  }
  if (this->regeneration_ != nullptr)
    this->regeneration_->publish_state(false);
  // Alerte sel mémorisée : survit aux redémarrages
  this->salt_pref_ = global_preferences->make_preference<uint8_t>(0x4A55444FUL);
  uint8_t v = 0;
  if (this->salt_pref_.load(&v))
    this->salt_latched_ = v != 0;
  if (this->flow_anomaly_ != nullptr)
    this->flow_anomaly_->publish_state(false);
  if (this->phantom_water_ != nullptr)
    this->phantom_water_->publish_state(0);
}

void JudoBus::loop() {
  if (this->flow_anomaly_on_ && (int32_t) (millis() - this->flow_anomaly_until_) >= 0) {
    this->flow_anomaly_on_ = false;
    if (this->flow_anomaly_ != nullptr)
      this->flow_anomaly_->publish_state(false);
    this->event_("Débit de nouveau cohérent depuis 30 min");
  }
  if (this->power_switch_ != nullptr)
    this->power_loop_();
  uint8_t tmp[64];
  while (this->available() > 0) {
    size_t n = std::min<size_t>(this->available(), sizeof(tmp));
    if (!this->read_array(tmp, n))
      break;
    if (this->buf_.size() + n > MAX_BUF)
      this->buf_.erase(this->buf_.begin(), this->buf_.begin() + (this->buf_.size() + n - MAX_BUF));
    this->buf_.insert(this->buf_.end(), tmp, tmp + n);
    if (this->log_raw_) {
      if (this->raw_burst_.empty())
        this->raw_burst_start_ms_ = millis();
      if (this->raw_burst_.size() < 256)
        this->raw_burst_.insert(this->raw_burst_.end(), tmp, tmp + std::min<size_t>(n, 256 - this->raw_burst_.size()));
    }
    if (this->trace_rx_total_ == 0)
      this->trace_rx_start_ms_ = millis();
    for (size_t i = 0; i < n; i++) {
      if (this->trace_rx_len_ < sizeof(this->trace_rx_))
        this->trace_rx_[this->trace_rx_len_++] = tmp[i];
    }
    this->trace_rx_total_ += n;
    this->last_rx_byte_ms_ = millis();
    this->rx_bytes_ += n;
  }
  if (this->trace_rx_total_ > 0 && millis() - this->last_rx_byte_ms_ >= 5)
    this->trace_flush_rx_();
  // Journal brut : chaque rafale d'octets (séparée par >= 5 ms de silence), décodable ou non
  if (this->log_raw_ && !this->raw_burst_.empty() && millis() - this->last_rx_byte_ms_ >= 5) {
    ESP_LOGI(TAG, "BRUT %3u o : %s", (unsigned) this->raw_burst_.size(),
             hex_(this->raw_burst_.data(), this->raw_burst_.size()).c_str());
    this->raw_burst_.clear();
  }
  this->parse_buffer_();
  if (this->master_)
    this->master_loop_();

  const uint32_t now = millis();
  // Diagnostic périodique (journal INFO + entité texte) toutes les 60 s
  if (now - this->last_diag_ms_ >= 60000) {
    this->last_diag_ms_ = now;
    char s[200];
    snprintf(s, sizeof(s), "%s | RX %" PRIu32 " o | trames OK %" PRIu32 " | CRC err %" PRIu32 " | TX %" PRIu32
             " | sans réponse %" PRIu32 " | reset module %" PRIu32 " | décrochages saumure %" PRIu32,
             this->master_ ? ((int32_t) (now - this->master_paused_until_) < 0 ? "maître (suspendu)" : "maître")
                           : "écoute",
             this->rx_bytes_, this->frames_ok_, this->crc_errors_, this->tx_frames_, this->timeouts_,
             this->power_cycles_, this->brine_dips_);
    ESP_LOGI(TAG, "Diagnostic bus : %s", s);
    if (this->bus_diagnostic_ != nullptr)
      this->bus_diagnostic_->publish_state(s);
    if (this->frames_ok_ == 0)
      ESP_LOGW(TAG, "Aucune trame valide reçue depuis le démarrage%s",
               this->rx_bytes_ == 0 ? " (aucun octet sur RX)" : " (octets reçus mais CRC/format invalides)");
  }
  if (now - this->last_check_ms_ < 5000)
    return;
  this->last_check_ms_ = now;

  // Santé du bus
  bool ok = this->last_valid_ms_ != 0 && (now - this->last_valid_ms_) < this->bus_timeout_ms_;
  if (!this->bus_state_known_ || ok != this->bus_state_) {
    this->bus_state_known_ = true;
    this->bus_state_ = ok;
    if (this->bus_ok_ != nullptr)
      this->bus_ok_->publish_state(ok);
    if (!ok && this->last_valid_ms_ != 0)
      this->event_("Bus RS485 muet depuis " + std::to_string(this->bus_timeout_ms_ / 60000) + " min");
    if (!ok && this->last_valid_ms_ != 0)
      this->trace_dump_("bus muet");
    if (!ok && !this->master_ && this->power_switch_ != nullptr &&
        (!this->power_cycled_once_ || now - this->last_power_cycle_ms_ > 300000))
      this->power_cycle_module("bus muet");
  }

  // Débit retombé à zéro sans nouveau litre depuis 3 min
  if (this->flow_rate_ != nullptr && this->water_known_ && now - this->water_change_ms_ > 180000 &&
      this->flow_rate_->has_state() && this->flow_rate_->state != 0.0f)
    this->flow_rate_->publish_state(0.0f);

  // Fin de la fenêtre « régénération »

}

void JudoBus::parse_buffer_() {
  auto &b = this->buf_;
  while (b.size() >= 8) {
    if (b[0] != this->address_ || b[1] != this->device_type_) {
      b.erase(b.begin());
      continue;
    }
    const uint16_t len = u16_(&b[4]);
    if (len > MAX_PAYLOAD) {
      b.erase(b.begin());
      continue;
    }
    const size_t total = 6 + len + 2;
    if (b.size() < total)
      return;  // trame incomplète : attendre la suite
    const uint16_t crc = crc16_(b.data(), 6 + len);
    if (crc != u16_(&b[6 + len])) {
      this->crc_errors_++;
      b.erase(b.begin());
      continue;
    }
    this->frames_ok_++;
    this->last_valid_ms_ = millis();
    this->handle_frame_(u16_(&b[2]), &b[6], len);
    b.erase(b.begin(), b.begin() + total);
  }
}

void JudoBus::handle_frame_(uint16_t cmd, const uint8_t *d, uint16_t len) {
  ESP_LOGV(TAG, "cmd 0x%04X len %u : %s", cmd, len, hex_(d, len).c_str());
  if (this->log_raw_) {
    // Journal brut de chaque trame valide, avec l'écart depuis la précédente (capture du démarrage du module)
    const uint32_t now = millis();
    ESP_LOGI(TAG, "TRAME +%5u ms  cmd 0x%04X  len %3u : %s", (unsigned) (now - this->last_raw_ms_), cmd, len,
             len > 40 ? (hex_(d, 40) + " ...").c_str() : hex_(d, len).c_str());
    this->last_raw_ms_ = now;
  }
  const bool is_request = len == 0 || (cmd == CMD_STATUS && len == 1);
  if (!is_request)
    this->trace_dumped_ = false;  // liaison vivante : une prochaine panne sera de nouveau tracée
  if (this->master_) {
    if (is_request && millis() - this->last_tx_ms_ > 100) {
      // Requête que nous n'avons pas émise : un autre maître (module Connectivity) est présent
      if ((int32_t) (millis() - this->master_paused_until_) >= 0)
        this->event_("Autre maître détecté sur le bus (module Connectivity ?) : interrogation suspendue 10 min");
      this->master_paused_until_ = millis() + 600000;
      this->awaiting_ = false;
    } else if (!is_request && this->awaiting_ && cmd == this->awaiting_cmd_) {
      this->awaiting_ = false;
      this->consecutive_timeouts_ = 0;
    } else if (!is_request && cmd == CMD_STATUS && (int32_t) (millis() - this->master_paused_until_) >= 0) {
      // Réponse 0x0043 tardive (après expiration du délai) : la page n'est plus sûre, on l'ignore
      // pour ne pas, par exemple, lire le bloc B comme un bloc A (fausse alerte sel).
      ESP_LOGD(TAG, "Réponse 0x0043 hors délai ignorée");
      return;
    }
  }
  if (len == 0)
    return;  // requête (module Connectivity ou écho de notre propre émission)

  switch (cmd) {
    case 0x0018:
      // [2-3] eau adoucie depuis minuit, en litres (vérifié : suit exactement le compteur 0x0029)
      if (len == 4) {
        if (this->water_today_ != nullptr)
          this->water_today_->publish_state(u16_(&d[2]));
        this->track_changes_(0x00180000, "commande 0x0018", d, len, {2, 3});
      }
      return;

    case CMD_SW_VERSION:
      if (len == 3 && this->software_version_ != nullptr) {
        char v[12];
        snprintf(v, sizeof(v), "%u.%02u", d[2], d[1]);
        if (!this->software_version_->has_state() || this->software_version_->state != v)
          this->software_version_->publish_state(v);
      }
      return;

    case CMD_HW_VERSION:
      if (len == 2 && this->hardware_version_ != nullptr) {
        char v[12];
        snprintf(v, sizeof(v), "%u.%02u", d[1], d[0]);
        if (!this->hardware_version_->has_state() || this->hardware_version_->state != v)
          this->hardware_version_->publish_state(v);
      }
      return;

    case CMD_DEVICE_NUMBER:
      if (len == 4 && this->device_number_ != nullptr)
        this->device_number_->publish_state(u32_(d));
      return;

    case CMD_HARDNESS_UNIT:
      if (len == 1 && this->hardness_unit_ != nullptr) {
        static const char *const units[] = {"°dH", "°e", "°f", "gpg", "ppm", "mmol/l", "mval/l"};
        std::string u = d[0] < 7 ? units[d[0]] : ("code " + std::to_string(d[0]));
        if (!this->hardness_unit_->has_state() || this->hardness_unit_->state != u)
          this->hardness_unit_->publish_state(u);
      }
      return;

    case CMD_OPERATING_TIME:
      if (len == 4 && this->operating_days_ != nullptr)
        this->operating_days_->publish_state(u16_(&d[2]) + d[1] / 24.0f + d[0] / 1440.0f);
      return;

    case CMD_MAINTENANCE:
      if (len >= 2) {
        const uint16_t hours = u16_(d);
        const float days = hours / 24.0f;
        if (this->maintenance_days_ != nullptr)
          this->maintenance_days_->publish_state(days);
        const bool due = hours == 0 || days <= (float) this->maint_warn_days_;
        if (this->maintenance_due_ != nullptr &&
            (!this->maintenance_due_->has_state() || this->maintenance_due_->state != due)) {
          this->maintenance_due_->publish_state(due);
          if (due)
            this->event_("Maintenance due");
        }
        if (len > 2)
          this->track_changes_(0x00260000, "maintenance", d, len, {0, 1});
      }
      return;

    case CMD_SOFT_WATER:
      if (len == 4) {
        const uint32_t litres = u32_(d);
        const uint32_t now = millis();
        if (this->water_known_ && litres > this->water_l_ && litres - this->water_l_ < 10000) {
          // Contrôle de cohérence : une hausse supérieure au débit maximal plausible
          // (max_flow_ L/min, avec 3 L de tolérance) est comptée comme « eau fantôme ».
          const uint32_t delta = litres - this->water_l_;
          const uint32_t dtw = now - this->water_poll_ms_;
          const uint32_t plausible = (uint32_t) ((uint64_t) this->max_flow_ * dtw / 60000ULL) + 3;
          if (dtw > 0 && dtw < 600000 && delta > plausible) {
            const uint32_t excess = delta - plausible;
            this->phantom_l_ += excess;
            if (this->phantom_water_ != nullptr)
              this->phantom_water_->publish_state(this->phantom_l_);
            ESP_LOGW(TAG, "Compteur d'eau incohérent : +%u L en %u ms (%.0f L/min)", (unsigned) delta,
                     (unsigned) dtw, delta * 60000.0f / dtw);
            if (!this->flow_anomaly_on_) {
              this->flow_anomaly_on_ = true;
              if (this->flow_anomaly_ != nullptr)
                this->flow_anomaly_->publish_state(true);
              char buf[96];
              snprintf(buf, sizeof(buf), "Débit incohérent : +%u L en %.0f s (turbine ?)", (unsigned) delta,
                       dtw / 1000.0f);
              this->event_(buf);
            }
            this->flow_anomaly_until_ = now + 1800000;  // l'alerte retombe après 30 min sans incohérence
          }
          // débit moyen depuis la lecture précédente (le module interroge environ chaque minute)
          const uint32_t dt = now - this->water_poll_ms_;
          if (this->flow_rate_ != nullptr && dt > 0 && dt < 600000)
            this->flow_rate_->publish_state((litres - this->water_l_) * 60000.0f / dt);
          this->water_change_ms_ = now;
        }
        if (!this->water_known_) {
          this->water_change_ms_ = now;
          if (this->flow_rate_ != nullptr)
            this->flow_rate_->publish_state(0.0f);
        }
        this->water_known_ = true;
        this->water_l_ = litres;
        this->water_poll_ms_ = now;
        if (this->soft_water_total_ != nullptr)
          this->soft_water_total_->publish_state(litres);
      }
      return;

    case CMD_RAW_HARDNESS:
      if (len == 2 && this->raw_hardness_ != nullptr)
        this->raw_hardness_->publish_state(u16_(d));
      return;

    case CMD_RAW_HARDNESS2:
      if (len == 2)
        this->track_changes_(0x00540000, "dureté (0x54)", d, len, {});
      return;

    case CMD_RAW_WATER:
      this->track_changes_(0x00280000, "eau brute (0x28)", d, len, {});
      return;

    case CMD_STATUS:
      this->handle_status_block_(d, len);
      return;

    default:
      // Commande non décodée : on enregistre toute évolution (utile pour la régénération)
      this->track_changes_((uint32_t) cmd << 16, "commande", d, len, {});
      return;
  }
}

void JudoBus::trace_push_(bool tx, const uint8_t *d, size_t len, uint32_t ms) {
  TraceEntry &e = this->trace_[this->trace_head_];
  e.ms = ms;
  e.tx = tx ? 1 : 0;
  e.len = len > 255 ? 255 : (uint8_t) len;
  const size_t n = len < sizeof(e.data) ? len : sizeof(e.data);
  memcpy(e.data, d, n);
  this->trace_head_ = (this->trace_head_ + 1) % TRACE_N;
  if (this->trace_count_ < TRACE_N)
    this->trace_count_++;
}

void JudoBus::trace_flush_rx_() {
  // trace_rx_ ne garde que les 24 premiers octets ; la longueur réelle est conservée
  TraceEntry &e = this->trace_[this->trace_head_];
  e.ms = this->trace_rx_start_ms_;
  e.tx = 0;
  e.len = this->trace_rx_total_ > 255 ? 255 : (uint8_t) this->trace_rx_total_;
  memcpy(e.data, this->trace_rx_, this->trace_rx_len_);
  this->trace_head_ = (this->trace_head_ + 1) % TRACE_N;
  if (this->trace_count_ < TRACE_N)
    this->trace_count_++;
  this->trace_rx_len_ = 0;
  this->trace_rx_total_ = 0;
}

void JudoBus::trace_dump_(const char *why) {
  if (this->trace_dumped_ || this->trace_count_ == 0)
    return;
  this->trace_dumped_ = true;
  const uint32_t now = millis();
  ESP_LOGE(TAG, "=== Boîte noire bus JUDO (%s) : %u derniers échanges ===", why, this->trace_count_);
  uint32_t prev = 0;
  for (uint8_t i = 0; i < this->trace_count_; i++) {
    const uint8_t idx = (this->trace_head_ + TRACE_N - this->trace_count_ + i) % TRACE_N;
    const TraceEntry &e = this->trace_[idx];
    const uint8_t shown = e.len < sizeof(e.data) ? e.len : sizeof(e.data);
    ESP_LOGE(TAG, "%c -%6u ms (+%4u) %3u o : %s%s", e.tx ? 'T' : 'R', (unsigned) (now - e.ms),
             (unsigned) (i == 0 ? 0 : e.ms - prev), e.len, hex_(e.data, shown).c_str(),
             e.len > shown ? " ..." : "");
    prev = e.ms;
  }
  ESP_LOGE(TAG, "=== fin de la boîte noire ===");
}

void JudoBus::power_cycle_module(const char *reason) {
  if (this->power_switch_ == nullptr || this->power_cycling_)
    return;
  const uint32_t now = millis();
  this->power_cycles_++;
  this->power_cycled_once_ = true;
  this->last_power_cycle_ms_ = now;
  this->power_cycling_ = true;
  this->power_restore_ms_ = now + 1000;  // 1 s hors tension
  this->awaiting_ = false;
  ESP_LOGW(TAG, "Redémarrage du module RS485 (%s)", reason);
  if (this->last_event_ != nullptr)
    this->last_event_->publish_state(std::string("Module RS485 redémarré : ") + reason);
  if (this->power_switch_cuts_)
    this->power_switch_->turn_on();
  else
    this->power_switch_->turn_off();
}

void JudoBus::power_loop_() {
  const uint32_t now = millis();
  if (this->boot_cycle_pending_ && now > 2000) {
    // Au démarrage de l'ESP : le module a vu GPIO15 à l'état bas pendant le boot, on le réinitialise
    this->boot_cycle_pending_ = false;
    this->power_cycle_module("démarrage de l'ESP");
    this->power_cycled_once_ = false;  // ne compte pas dans la limitation à 1 reset / 5 min
  }
  if (this->power_cycling_ && (int32_t) (now - this->power_restore_ms_) >= 0) {
    if (this->power_switch_cuts_)
      this->power_switch_->turn_off();
    else
      this->power_switch_->turn_on();
    this->power_cycling_ = false;
    this->consecutive_timeouts_ = 0;
    this->buf_.clear();
    this->master_paused_until_ = now + 1500;  // laisser le module se stabiliser
    ESP_LOGI(TAG, "Module RS485 réalimenté");
  }
}

void JudoBus::set_salt_latched_(bool on) {
  if (on == this->salt_latched_ && this->salt_published_)
    return;
  const bool changed = on != this->salt_latched_;
  this->salt_latched_ = on;
  if (changed) {
    uint8_t v = on ? 1 : 0;
    this->salt_pref_.save(&v);
  }
  if (this->salt_shortage_ != nullptr) {
    this->salt_shortage_->publish_state(on);
    this->salt_published_ = true;
  }
}

void JudoBus::reset_salt_alarm() {
  ESP_LOGI(TAG, "Alerte sel réinitialisée manuellement");
  this->set_salt_latched_(false);
  if (this->last_event_ != nullptr)
    this->last_event_->publish_state("Alerte sel réinitialisée manuellement");
}

void JudoBus::send_request_(uint16_t cmd, const uint8_t *data, uint16_t len) {
  uint8_t f[6 + 8 + 2];
  if (len > 8)
    return;
  f[0] = this->address_;
  f[1] = this->device_type_;
  f[2] = cmd & 0xFF;
  f[3] = cmd >> 8;
  f[4] = len & 0xFF;
  f[5] = len >> 8;
  for (uint16_t i = 0; i < len; i++)
    f[6 + i] = data[i];
  const uint16_t crc = crc16_(f, 6 + len);
  f[6 + len] = crc & 0xFF;
  f[7 + len] = crc >> 8;
  if (this->tx_uart_ != nullptr) {
    this->tx_uart_->write_array(f, 8 + len);
    this->tx_uart_->flush();
  } else {
    this->write_array(f, 8 + len);
    this->flush();
  }
  this->trace_push_(true, f, 8 + len, millis());
  this->last_tx_ms_ = millis();
  this->tx_frames_++;
  this->awaiting_ = true;
  this->awaiting_cmd_ = cmd;
  ESP_LOGV(TAG, "TX %s", hex_(f, 8 + len).c_str());
}

void JudoBus::master_loop_() {
  const uint32_t now = millis();
  if (this->power_cycling_ || (int32_t) (now - this->master_paused_until_) < 0)
    return;
  if (this->awaiting_) {
    if (now - this->last_tx_ms_ < 400)
      return;  // réponse attendue
    this->awaiting_ = false;
    this->last_page_ = -1;
    this->timeouts_++;
    if (++this->consecutive_timeouts_ == 10) {
      ESP_LOGW(TAG, "Aucune réponse de l'adoucisseur à 10 requêtes (câblage TX, A/B, ou session requise ?)");
      this->trace_dump_("10 requêtes sans réponse");
    }
    // Après une absence de réponse, laisser au moins 150 ms de silence avant de réémettre :
    // une réponse tardive de l'adoucisseur ne doit pas croiser notre requête suivante.
    this->tx_hold_until_ = now + 150;
    if (this->power_switch_ != nullptr && this->consecutive_timeouts_ >= this->stuck_threshold_ &&
        (!this->power_cycled_once_ || now - this->last_power_cycle_ms_ > 300000)) {
      this->power_cycle_module("pas de réponse de l'adoucisseur");
      return;
    }
  }
  // Anti-collision : bus libre depuis 50 ms, aucune trame en cours de réception,
  // délai de garde après une absence de réponse, intervalle minimal entre deux requêtes.
  if (now - this->last_rx_byte_ms_ < 50 || now - this->last_tx_ms_ < this->poll_interval_ms_ ||
      (int32_t) (now - this->tx_hold_until_) < 0)
    return;
  if (!this->buf_.empty() && now - this->last_rx_byte_ms_ < 300)
    return;  // trame partielle dans le tampon : on attend qu'elle se termine ou expire
  if (!this->buf_.empty())
    this->buf_.clear();  // reliquat d'une trame incomplète

  if (this->session_init_ && !this->session_done_) {
    // Écritures observées depuis le module Connectivity (rôle inconnu)
    static const uint8_t init[4] = {0x60, 0x73, 0x50, 0x06};
    static uint8_t step = 0;
    this->send_request_(step == 0 ? 0x000D : 0x000E, init, 4);
    this->awaiting_ = false;
    if (++step >= 2)
      this->session_done_ = true;
    return;
  }

  PollItem *best = nullptr;
  for (auto &p : this->poll_) {
    if ((int32_t) (now - p.next_ms) >= 0 && (best == nullptr || (int32_t) (p.next_ms - best->next_ms) < 0))
      best = &p;
  }
  if (best == nullptr)
    return;
  best->next_ms = now + best->period_ms;
  if (best->page >= 0) {
    const uint8_t pg = (uint8_t) best->page;
    this->last_page_ = pg;  // pas d'écho garanti : on note la page nous-mêmes
    this->last_page_ms_ = now;
    this->send_request_(best->cmd, &pg, 1);
  } else {
    this->send_request_(best->cmd, nullptr, 0);
  }
}

void JudoBus::handle_status_block_(const uint8_t *d, uint16_t len) {
  // Requête du module : 1 octet = numéro de page (0..4 observés, un cycle par minute)
  if (len == 1) {
    if (d[0] > 4 && this->log_changes_)
      ESP_LOGI(TAG, "[0x0043] page inconnue demandée : %u", d[0]);
    this->last_page_ = d[0];
    this->last_page_ms_ = millis();
    return;
  }

  // Réponse : classement par la page demandée juste avant (sinon par la forme du bloc)
  int page = -1;
  if (this->last_page_ >= 0 && millis() - this->last_page_ms_ < 1000)
    page = this->last_page_;
  this->last_page_ = -1;

  if (page == 3 || page == 4 || (page < 0 && len == 64)) {
    const bool codes = page == 4 || (page < 0 && d[0] == 0 && d[1] == 0 && d[3] == 0x01);
    if (codes) {
      this->track_changes_(0x00430000 | BLK_HIST_CODES, "historique (codes)", d, len, {});
      if (this->message_history_ != nullptr) {
        uint8_t last_code = 0, last_idx = 0;
        for (int i = 2; i + 1 < (int) len; i += 2) {
          if (d[i] == 0 && d[i + 1] == 0)
            break;
          last_code = d[i];
          last_idx = d[i + 1];
        }
        char s[64];
        snprintf(s, sizeof(s), "%u messages, dernier code 0x%02X", last_idx, last_code);
        if (!this->message_history_->has_state() || this->message_history_->state != s)
          this->message_history_->publish_state(s);
      }
    } else {
      this->track_changes_(0x00430000 | BLK_HIST_DAYS, "historique (jours)", d, len, {});
    }
    return;
  }

  if (page > 4) {
    this->track_changes_(0x00430000 | (0x10 + page), "0x43 page inconnue", d, len, {});
    return;
  }

  if (len != 32) {
    this->track_changes_(0x00430000 | BLK_UNKNOWN, "0x43 inconnu", d, len, {});
    return;
  }

  // Bloc B : 00 81 0F 10 …
  if (page == 0 || (page < 0 && d[1] == 0x81)) {
    // [18-19] débit instantané en centilitres/min (1998 = 19,98 L/min, cohérent avec le compteur)
    if (this->flow_indicator_ != nullptr)
      this->flow_indicator_->publish_state(d[18]);
    if (this->flow_instant_ != nullptr)
      this->flow_instant_->publish_state(u16_(&d[18]) / 100.0f);
    this->track_changes_(0x00430000 | BLK_B, "bloc B", d, len, {18, 19, 28});
    return;
  }

  // Bloc C : 00 xx 00 … 00 [18]=80 …
  if (page == 2 || (page < 0 && d[18] == 0x80 && d[26] == 0 && d[30] == 0 && d[31] == 0)) {
    this->track_changes_(0x00430000 | BLK_C, "bloc C", d, len, {1, 24});
    return;
  }

  // Bloc A : état principal
  //  [0] bit7 = électrode de saumure immergée, bit0 = régénération en cours
  //  [6-7] compteur de capacité (+17/L, remis à 0 au début de la régénération)
  //  [14-15] secondes restantes de la phase de régénération en cours
  //  [26] = 14 au repos (dureté résiduelle ?), varie pendant la régénération
  //  [30-31] nombre de régénérations
  if (page == 1 || (page < 0 && d[1] == 0 && d[2] == 0 && d[3] == 0 && u16_(&d[30]) != 0)) {
    const bool brine_ok = (d[0] & 0x80) != 0;
    const bool in_regen = (d[0] & 0x01) != 0;

    // --- Régénération en temps réel (octet 0, bit 0) ---
    if (!this->regen_bit_known_ || in_regen != this->regen_bit_) {
      if (this->regen_bit_known_) {
        if (in_regen) {
          this->regen_salt_drop_ = false;
          this->event_("Régénération démarrée");
        } else if (this->regen_salt_drop_ || (!brine_ok && this->brine_lost_ms_ != 0)) {
          this->set_salt_latched_(true);
          this->event_("Régénération terminée : saumure insuffisante pendant l'aspiration, ajouter du sel");
        } else {
          this->event_("Régénération terminée : saumure correcte");
          this->set_salt_latched_(false);
        }
      }
      this->regen_bit_known_ = true;
      this->regen_bit_ = in_regen;
      if (this->regeneration_ != nullptr)
        this->regeneration_->publish_state(in_regen);
    }
    if (this->regeneration_remaining_ != nullptr) {
      const uint16_t secs = in_regen ? u16_(&d[14]) : 0;
      this->regeneration_remaining_->publish_state((secs + 59) / 60);
    }

    // --- Électrode de saumure + alerte sel mémorisée (temporisée) ---
    // Pendant l'aspiration de la saumure, l'électrode décroche normalement 2 à 4 s :
    // on n'alerte que si elle reste hors saumure au moins salt_delay_ms_.
    const uint32_t now_ms = millis();
    if (!this->brine_known_ || brine_ok != this->brine_ok_) {
      if (this->brine_known_) {
        if (!brine_ok) {
          this->brine_lost_ms_ = now_ms == 0 ? 1 : now_ms;
          ESP_LOGD(TAG, "Électrode hors saumure (%s)", in_regen ? "régénération" : "repos");
        } else if (this->brine_lost_ms_ != 0) {
          const uint32_t lost = now_ms - this->brine_lost_ms_;
          if (lost < this->salt_delay_ms_) {
            this->brine_dips_++;
            ESP_LOGI(TAG, "Décrochage bref de l'électrode de saumure (%u s), ignoré", (unsigned) (lost / 1000));
          } else {
            this->event_("Électrode de saumure de nouveau immergée");
          }
          this->brine_lost_ms_ = 0;
        }
      } else if (!brine_ok) {
        this->brine_lost_ms_ = now_ms == 0 ? 1 : now_ms;
      }
      this->brine_known_ = true;
      this->brine_ok_ = brine_ok;
    }
    if (!brine_ok && this->brine_lost_ms_ != 0 && now_ms - this->brine_lost_ms_ >= this->salt_delay_ms_) {
      if (!this->salt_latched_)
        this->event_(in_regen ? "Saumure absente pendant la régénération : manque de sel"
                              : "Électrode de saumure hors saumure depuis plus de " +
                                    std::to_string(this->salt_delay_ms_ / 60000) + " min : manque de sel");
      if (in_regen)
        this->regen_salt_drop_ = true;
      this->set_salt_latched_(true);
    }
    if (this->salt_shortage_ != nullptr && !this->salt_published_) {
      this->salt_shortage_->publish_state(this->salt_latched_);
      this->salt_published_ = true;
    }

    // --- Capacité ---
    const uint16_t cap = u16_(&d[6]);
    if (this->capacity_known_ && this->last_capacity_ > 500 && cap + 500 < this->last_capacity_) {
      ESP_LOGI(TAG, "Compteur de capacité remis à zéro à %u (début de régénération)", this->last_capacity_);
      if (this->capacity_at_regeneration_ != nullptr)
        this->capacity_at_regeneration_->publish_state(this->last_capacity_);
    }
    this->capacity_known_ = true;
    this->last_capacity_ = cap;
    if (this->capacity_counter_ != nullptr)
      this->capacity_counter_->publish_state(cap);
    if (this->capacity_remaining_ != nullptr && this->capacity_threshold_ > 0) {
      float pct = 100.0f - 100.0f * cap / this->capacity_threshold_;
      this->capacity_remaining_->publish_state(pct < 0 ? 0.0f : pct);
    }

    if (this->status_countdown_ != nullptr)
      this->status_countdown_->publish_state(d[4]);
    if (this->residual_hardness_ != nullptr && !in_regen)
      this->residual_hardness_->publish_state(d[26]);

    const uint16_t regen = u16_(&d[30]);
    if (this->regeneration_count_ != nullptr)
      this->regeneration_count_->publish_state(regen);
    if (this->regen_known_ && regen != this->regen_count_)
      this->event_("Compteur de régénérations : " + std::to_string(this->regen_count_) + " → " +
                   std::to_string(regen));
    this->regen_known_ = true;
    this->regen_count_ = regen;

    // octets connus et volatils
    this->track_changes_(0x00430000 | BLK_A, "bloc A", d, len,
                         {0, 1, 2, 4, 5, 6, 7, 8, 9, 14, 15, 16, 17, 21, 30, 31});
    return;
  }

  this->track_changes_(0x00430000 | BLK_UNKNOWN, "0x43 non classé", d, len, {});
}

void JudoBus::track_changes_(uint32_t key, const char *label, const uint8_t *d, uint16_t len,
                             const std::vector<uint8_t> &volatile_idx) {
  auto it = this->last_payload_.find(key);
  if (it == this->last_payload_.end()) {
    if (this->last_payload_.size() < 64)
      this->last_payload_[key] = std::vector<uint8_t>(d, d + len);
    if (this->log_changes_)
      ESP_LOGI(TAG, "[%s 0x%04X] première lecture : %s", label, (unsigned) (key >> 16), hex_(d, len).c_str());
    return;
  }
  auto &prev = it->second;
  if (prev.size() != len) {
    if (this->log_changes_)
      ESP_LOGI(TAG, "[%s 0x%04X] longueur %u → %u : %s", label, (unsigned) (key >> 16), (unsigned) prev.size(), len,
               hex_(d, len).c_str());
    prev.assign(d, d + len);
    return;
  }
  std::string diff;
  bool significant = false;
  for (uint16_t i = 0; i < len; i++) {
    if (prev[i] == d[i])
      continue;
    bool vol = false;
    for (auto v : volatile_idx)
      if (v == i)
        vol = true;
    if (!vol)
      significant = true;
    char s[24];
    snprintf(s, sizeof(s), " [%u]%02X→%02X%s", i, prev[i], d[i], vol ? "" : "*");
    diff += s;
  }
  if (!diff.empty() && significant && this->unknown_change_ != nullptr) {
    // Historisé par HA (recorder) : survit à la rotation du journal
    char head[48];
    snprintf(head, sizeof(head), "%s 0x%04X :", label, (unsigned) (key >> 16));
    std::string msg = std::string(head) + diff;
    if (msg.size() > 250)
      msg = msg.substr(0, 247) + "...";
    this->unknown_change_->publish_state(msg);
  }
  if (!diff.empty() && this->log_changes_) {
    if (significant)
      ESP_LOGI(TAG, "[%s 0x%04X] évolution (* = champ inconnu) :%s", label, (unsigned) (key >> 16), diff.c_str());
    else
      ESP_LOGD(TAG, "[%s 0x%04X] :%s", label, (unsigned) (key >> 16), diff.c_str());
  }
  prev.assign(d, d + len);
}

void JudoBus::event_(const std::string &msg) {
  ESP_LOGW(TAG, "%s", msg.c_str());
  if (this->last_event_ != nullptr)
    this->last_event_->publish_state(msg);
}

void JudoBus::dump_config() {
  ESP_LOGCONFIG(TAG, "Bus JUDO :");
  ESP_LOGCONFIG(TAG, "  Mode : %s", this->master_ ? "MAÎTRE (interroge l'adoucisseur)" : "écoute seule");
  if (this->master_)
    ESP_LOGCONFIG(TAG, "  Intervalle entre requêtes : %u ms, init session : %s", (unsigned) this->poll_interval_ms_,
                  this->session_init_ ? "oui" : "non");
  if (this->master_)
    ESP_LOGCONFIG(TAG, "  Émission : %s", this->tx_uart_ != nullptr ? "UART séparé" : "UART principal");
  ESP_LOGCONFIG(TAG, "  Adresse 0x%02X, type d'appareil 0x%02X", this->address_, this->device_type_);
  ESP_LOGCONFIG(TAG, "  Trames valides : %" PRIu32 ", erreurs CRC : %" PRIu32 ", requêtes sans réponse : %" PRIu32,
                this->frames_ok_, this->crc_errors_, this->timeouts_);
}

}  // namespace judo_bus
}  // namespace esphome
