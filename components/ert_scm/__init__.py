# ERT-SCM decode + optional MQTT publish. See README.md and docs/PLAN.md.
#
# When an MQTT client is configured, publishes fixed meters/gas/{id}/… topics
# (no MQTT discovery). HA-native API sensors work without MQTT.

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import remote_base, sensor, sx127x, time
from esphome.const import (
    CONF_ID,
    CONF_TIME_ID,
    DEVICE_CLASS_GAS,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_DECIBEL_MILLIWATT,
)

CODEOWNERS = ["@shomanjk"]
# MQTT is optional: HA API-only examples omit it; publish_() no-ops then.
DEPENDENCIES = ["remote_receiver", "sx127x", "time"]
AUTO_LOAD = ["sensor"]

ert_scm_ns = cg.esphome_ns.namespace("ert_scm")
# Registered as a primary remote_receiver *dumper*, not a listener. remote_base
# ignores listener return values, but a primary dumper returning true stops
# the secondary `raw` dumper from also printing decoded captures.
ErtScmComponent = ert_scm_ns.class_(
    "ErtScmComponent", cg.Component, remote_base.RemoteReceiverDumperBase
)

CONF_SX127X_ID = "sx127x_id"
CONF_SCALE = "scale"
CONF_MQTT_TOPIC_PREFIX = "mqtt_topic_prefix"
CONF_METERS = "meters"
CONF_METER_ID = "id"
CONF_NAME = "name"
CONF_ENABLED = "enabled"
CONF_CONSUMPTION = "consumption"
CONF_HISTORY = "history"
CONF_RSSI = "rssi"

METER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_METER_ID): cv.int_range(min=1),
        cv.Optional(CONF_NAME, default=""): cv.string,
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        # Local copy of the published reading (raw ÷ scale), for the OLED or
        # the native API. The ERT counts cubic feet, so the default scale of
        # 100 gives CCF (hundreds of cubic feet); override
        # unit_of_measurement for other scales or meters.
        cv.Optional(CONF_CONSUMPTION): sensor.sensor_schema(
            unit_of_measurement="CCF",
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_GAS,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        # Rolling 24 h of hourly usage, saved to flash on each reading, for the
        # OLED's graph and "today" fallback. Off by default so neighbors don't write.
        cv.Optional(CONF_HISTORY, default=False): cv.boolean,
        # Signal strength of each frame from this meter, from the SX1276's
        # live RSSI register (not published on the meters/gas MQTT topics).
        cv.Optional(CONF_RSSI): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ErtScmComponent),
            cv.GenerateID(CONF_SX127X_ID): cv.use_id(sx127x.SX127x),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_SCALE, default=100): cv.positive_int,
            cv.Optional(CONF_MQTT_TOPIC_PREFIX, default="meters/gas"): cv.string,
            cv.Optional(CONF_METERS, default=[]): cv.ensure_list(METER_SCHEMA),
        }
    )
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)



CONF_FILTER_SYMBOLS = "filter_symbols"


def _final_validate(config):
    # ert_scm must see every capture: each one bounds the RSSI window of the
    # next, and a short burst dropped by remote_receiver's filter_symbols could
    # otherwise be reported as the following frame's signal strength. ert_scm
    # drops short captures itself instead.
    full_config = fv.full_config.get()
    receiver_id = config[remote_base.CONF_RECEIVER_ID]
    receiver = full_config.get_config_for_path(full_config.get_path_for_id(receiver_id)[:-1])
    if receiver.get(CONF_FILTER_SYMBOLS, 0):
        raise cv.Invalid(
            f"ert_scm needs every capture from remote_receiver '{receiver_id}': remove "
            f"`filter_symbols` (or set it to 0). ert_scm drops captures too short to "
            f"be an SCM frame itself, so it can keep RSSI windows accurate."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    receiver = await cg.get_variable(config[remote_base.CONF_RECEIVER_ID])
    cg.add(receiver.register_dumper(var))
    radio = await cg.get_variable(config[CONF_SX127X_ID])
    cg.add(var.set_radio(radio))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))
    cg.add(var.set_scale(config[CONF_SCALE]))
    cg.add(var.set_mqtt_topic_prefix(config[CONF_MQTT_TOPIC_PREFIX]))
    for meter in config[CONF_METERS]:
        consumption = cg.nullptr
        if CONF_CONSUMPTION in meter:
            consumption = await sensor.new_sensor(meter[CONF_CONSUMPTION])
        rssi = cg.nullptr
        if CONF_RSSI in meter:
            rssi = await sensor.new_sensor(meter[CONF_RSSI])
        cg.add(
            var.add_meter(
                meter[CONF_METER_ID],
                meter[CONF_NAME],
                meter[CONF_ENABLED],
                consumption,
                meter[CONF_HISTORY],
                rssi,
            )
        )
