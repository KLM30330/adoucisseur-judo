"""Décodeur passif du bus RS485 JUDO (Quicksoft-M, type 0x38) pour ESPHome.

Écoute seule par défaut (master: true pour interroger l'adoucisseur) : reconstruit les trames 01 38 | cmd | len | data | CRC16-CCITT,
publie les valeurs identifiées et journalise toute évolution inconnue.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, switch, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_VOLUME_FLOW_RATE,
    DEVICE_CLASS_WATER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "switch"]

judo_ns = cg.esphome_ns.namespace("judo_bus")
JudoBus = judo_ns.class_("JudoBus", cg.Component, uart.UARTDevice)

CONF_ADDRESS = "address"
CONF_DEVICE_TYPE = "device_type"
CONF_BUS_TIMEOUT = "bus_timeout"
CONF_REGEN_HOLD = "regeneration_hold"
CONF_MAINT_WARN_DAYS = "maintenance_warning_days"
CONF_LOG_CHANGES = "log_unknown_changes"
CONF_MASTER = "master"
CONF_POLL_INTERVAL = "poll_interval"
CONF_SESSION_INIT = "session_init"
CONF_CAPACITY_THRESHOLD = "capacity_threshold"
CONF_TX_UART_ID = "tx_uart_id"
CONF_LOG_RAW = "log_raw_frames"
CONF_POWER_SWITCH = "power_switch"
CONF_POWER_SWITCH_CUTS = "power_switch_cuts"
CONF_POWER_CYCLE_ON_BOOT = "power_cycle_on_boot"
CONF_STUCK_TIMEOUTS = "stuck_timeouts"
CONF_SALT_DELAY = "salt_alarm_delay"
CONF_MAX_FLOW = "max_flow"

DIAG = ENTITY_CATEGORY_DIAGNOSTIC

SENSORS = {
    "soft_water_total": dict(
        unit_of_measurement="L",
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_WATER,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        icon="mdi:water",
    ),
    "flow_rate": dict(
        unit_of_measurement="L/min",
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_VOLUME_FLOW_RATE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "regeneration_count": dict(
        accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING, icon="mdi:autorenew"
    ),
    "raw_hardness": dict(accuracy_decimals=0, icon="mdi:water-opacity"),
    "residual_hardness": dict(accuracy_decimals=0, icon="mdi:water-opacity"),
    "maintenance_days": dict(
        unit_of_measurement="d", accuracy_decimals=1, icon="mdi:wrench-clock"
    ),
    "operating_days": dict(
        unit_of_measurement="d", accuracy_decimals=2, entity_category=DIAG, icon="mdi:timer-outline"
    ),
    "device_number": dict(accuracy_decimals=0, entity_category=DIAG, icon="mdi:identifier"),
    # Champs encore expérimentaux (signification à confirmer)
    "status_countdown": dict(unit_of_measurement="h", accuracy_decimals=0, entity_category=DIAG),
    "capacity_counter": dict(accuracy_decimals=0, entity_category=DIAG),
    "flow_indicator": dict(accuracy_decimals=0, entity_category=DIAG),
    # Eau du jour (0x0018) et débit instantané (bloc B [18-19])
    "water_today": dict(
        unit_of_measurement="L", accuracy_decimals=0, device_class=DEVICE_CLASS_WATER,
        state_class=STATE_CLASS_TOTAL_INCREASING, icon="mdi:water-outline",
    ),
    "flow_instant": dict(
        unit_of_measurement="L/min", accuracy_decimals=1, state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:waves-arrow-right",
    ),
    # Régénération et capacité
    "regeneration_remaining": dict(
        unit_of_measurement="min", accuracy_decimals=0, icon="mdi:timer-sand"
    ),
    "capacity_remaining": dict(
        unit_of_measurement="%", accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:gauge",
    ),
    "capacity_at_regeneration": dict(accuracy_decimals=0, entity_category=DIAG, icon="mdi:gauge-full"),
    # Litres comptés de façon physiquement impossible (rafales de la turbine)
    "phantom_water": dict(
        unit_of_measurement="L", accuracy_decimals=0, state_class="total_increasing",
        icon="mdi:water-alert",
    ),
}

BINARY_SENSORS = {
    "salt_shortage": dict(device_class=DEVICE_CLASS_PROBLEM, icon="mdi:shaker-outline"),
    "maintenance_due": dict(device_class=DEVICE_CLASS_PROBLEM, icon="mdi:wrench-clock"),
    "regeneration": dict(device_class=DEVICE_CLASS_RUNNING, icon="mdi:autorenew"),
    "bus_ok": dict(device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=DIAG),
    "flow_anomaly": dict(device_class=DEVICE_CLASS_PROBLEM, icon="mdi:water-alert"),
}

TEXT_SENSORS = {
    "hardware_version": dict(entity_category=DIAG, icon="mdi:chip"),
    "software_version": dict(entity_category=DIAG, icon="mdi:application-cog"),
    "hardness_unit": dict(entity_category=DIAG),
    "message_history": dict(entity_category=DIAG, icon="mdi:history"),
    "last_event": dict(icon="mdi:bell-outline"),
    "unknown_change": dict(entity_category=DIAG, icon="mdi:magnify-scan"),
    "bus_diagnostic": dict(entity_category=DIAG, icon="mdi:lan-pending"),
}

_schema = {
    cv.GenerateID(): cv.declare_id(JudoBus),
    cv.Optional(CONF_ADDRESS, default=0x01): cv.hex_uint8_t,
    cv.Optional(CONF_DEVICE_TYPE, default=0x38): cv.hex_uint8_t,
    cv.Optional(CONF_BUS_TIMEOUT, default="5min"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_REGEN_HOLD, default="90min"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_MAINT_WARN_DAYS, default=0): cv.int_range(min=0, max=365),
    cv.Optional(CONF_LOG_CHANGES, default=True): cv.boolean,
    # Mode maître : l'ESP interroge lui-même l'adoucisseur (module Connectivity retiré)
    cv.Optional(CONF_MASTER, default=False): cv.boolean,
    cv.Optional(CONF_POLL_INTERVAL, default="500ms"): cv.All(
        cv.positive_time_period_milliseconds,
        cv.Range(min=cv.TimePeriod(milliseconds=100)),
    ),
    cv.Optional(CONF_SESSION_INIT, default=False): cv.boolean,
    # Valeur du compteur de capacité à laquelle l'adoucisseur régénère (≈ 3000 observé)
    cv.Optional(CONF_CAPACITY_THRESHOLD, default=3000): cv.int_range(min=100, max=65535),
    # UART d'émission séparé (mode maître) : ex. UART1 en émission seule sur GPIO2 (ESP8266)
    cv.Optional(CONF_TX_UART_ID): cv.use_id(uart.UARTComponent),
    # Journalise chaque trame valide (INFO) : capture de séquences (démarrage du module, etc.)
    cv.Optional(CONF_LOG_RAW, default=False): cv.boolean,
    # Alimentation du module RS485 commandée par un switch (relais NF, MOSFET...) :
    # redémarrage au boot de l'ESP et automatiquement si le module se bloque
    cv.Optional(CONF_POWER_SWITCH): cv.use_id(switch.Switch),
    cv.Optional(CONF_POWER_SWITCH_CUTS, default=True): cv.boolean,  # switch ON = module coupé
    cv.Optional(CONF_POWER_CYCLE_ON_BOOT, default=True): cv.boolean,
    cv.Optional(CONF_STUCK_TIMEOUTS, default=20): cv.int_range(min=3, max=1000),
    # Électrode hors saumure pendant au moins ce délai avant d'alerter (les décrochages
    # de quelques secondes pendant l'aspiration de la saumure sont normaux)
    cv.Optional(CONF_SALT_DELAY, default="2min"): cv.positive_time_period_milliseconds,
    # Débit au-delà duquel une hausse du compteur d'eau est jugée impossible (L/min)
    cv.Optional(CONF_MAX_FLOW, default=60): cv.int_range(min=10, max=1000),
}
for _k, _kw in SENSORS.items():
    _schema[cv.Optional(_k)] = sensor.sensor_schema(**_kw)
for _k, _kw in BINARY_SENSORS.items():
    _schema[cv.Optional(_k)] = binary_sensor.binary_sensor_schema(**_kw)
for _k, _kw in TEXT_SENSORS.items():
    _schema[cv.Optional(_k)] = text_sensor.text_sensor_schema(**_kw)

CONFIG_SCHEMA = (
    cv.Schema(_schema).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)
)

_uart_listen = uart.final_validate_device_schema(
    "judo_bus", baud_rate=19200, require_rx=True, data_bits=8, parity="NONE", stop_bits=1
)
_uart_master = uart.final_validate_device_schema(
    "judo_bus", baud_rate=19200, require_rx=True, require_tx=True, data_bits=8, parity="NONE", stop_bits=1
)


def FINAL_VALIDATE_SCHEMA(config):
    if config.get(CONF_MASTER) and CONF_TX_UART_ID not in config:
        return _uart_master(config)
    return _uart_listen(config)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_device_type(config[CONF_DEVICE_TYPE]))
    cg.add(var.set_bus_timeout(config[CONF_BUS_TIMEOUT]))
    cg.add(var.set_regeneration_hold(config[CONF_REGEN_HOLD]))
    cg.add(var.set_maintenance_warning_days(config[CONF_MAINT_WARN_DAYS]))
    cg.add(var.set_log_changes(config[CONF_LOG_CHANGES]))
    cg.add(var.set_master(config[CONF_MASTER]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_session_init(config[CONF_SESSION_INIT]))
    cg.add(var.set_capacity_threshold(config[CONF_CAPACITY_THRESHOLD]))
    cg.add(var.set_log_raw_frames(config[CONF_LOG_RAW]))
    cg.add(var.set_salt_alarm_delay(config[CONF_SALT_DELAY]))
    cg.add(var.set_max_flow(config[CONF_MAX_FLOW]))
    if CONF_POWER_SWITCH in config:
        ps = await cg.get_variable(config[CONF_POWER_SWITCH])
        cg.add(var.set_power_switch(ps))
        cg.add(var.set_power_switch_cuts(config[CONF_POWER_SWITCH_CUTS]))
        cg.add(var.set_power_cycle_on_boot(config[CONF_POWER_CYCLE_ON_BOOT]))
        cg.add(var.set_stuck_timeouts(config[CONF_STUCK_TIMEOUTS]))
    if CONF_TX_UART_ID in config:
        tx = await cg.get_variable(config[CONF_TX_UART_ID])
        cg.add(var.set_tx_uart(tx))

    for key in SENSORS:
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(s))
    for key in BINARY_SENSORS:
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_binary_sensor")(s))
    for key in TEXT_SENSORS:
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_text_sensor")(s))
