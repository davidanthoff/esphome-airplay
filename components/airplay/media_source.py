"""``media_source: - platform: airplay``

Modelled on ESPHome's own media sources (esphome/components/audio_http/
media_source.py and sendspin/media_source/__init__.py, 2026.9 / dev).
"""

from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32, media_source, network, socket
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
from esphome.core import CORE
from esphome.types import ConfigType

from . import airplay_ns

CODEOWNERS = ["@davidanthoff"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["audio", "mdns"]

CONF_OUTPUT_DELAY = "output_delay"
CONF_AIRPLAY_1_ONLY = "airplay_1_only"
CONF_TIMING_THRESHOLD = "timing_threshold"
CONF_REALTIME_TIMING_THRESHOLD = "realtime_timing_threshold"
CONF_ARTWORK = "artwork"

AirPlayMediaSource = airplay_ns.class_(
    "AirPlayMediaSource", cg.Component, media_source.MediaSource
)

# <repo>/components/airplay/media_source.py -> <repo>/airplay_core
# ESPHome clones the whole repository for external_components, so the sibling
# directory is present next to components/ both for git and local sources.
AIRPLAY_CORE_DIR = Path(__file__).resolve().parents[2] / "airplay_core"


def _consume_sockets(config: ConfigType) -> ConfigType:
    """Reserve lwIP sockets so ESPHome sizes CONFIG_LWIP_MAX_SOCKETS for us.

    Estimates from reading upstream (verify in M1 with lwIP stats):
      TCP listen: RTSP :7000, AirPlay 2 event port, buffered-audio port
      TCP:        RTSP client (current + old slot), event conn, buffered-audio conn
      UDP:        realtime data/control/timing, PTP 319 + 320, NTP
    Plus one TCP for the DACP HTTP client (AirPlay 1 remote control).
    """
    socket.consume_sockets(3, "airplay", socket.SocketType.TCP_LISTEN)(config)
    socket.consume_sockets(5, "airplay", socket.SocketType.TCP)(config)
    socket.consume_sockets(6, "airplay", socket.SocketType.UDP)(config)
    return config


def _request_networking(config: ConfigType) -> ConfigType:
    # Same as Sendspin: bigger lwIP buffers for streaming audio.
    network.require_high_performance_networking()
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(AirPlayMediaSource)
    .extend(
        {
            # Name in the iOS AirPlay picker. Defaults to the node's friendly_name.
            cv.Optional(CONF_NAME): cv.All(cv.string_strict, cv.Length(max=64)),
            # Delay added AFTER the ESP32 (TOSLINK receiver, AVR DSP). The timing
            # engine plays this much earlier. Tune against a HomePod.
            cv.Optional(CONF_OUTPUT_DELAY, default="0ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=500)),
            ),
            cv.Optional(CONF_AIRPLAY_1_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_TIMING_THRESHOLD, default="25ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=2),
                    max=cv.TimePeriod(milliseconds=200),
                ),
            ),
            cv.Optional(CONF_REALTIME_TIMING_THRESHOLD, default="50ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=2),
                    max=cv.TimePeriod(milliseconds=500),
                ),
            ),
            cv.Optional(CONF_ARTWORK, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _consume_sockets,
    _request_networking,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    name = config.get(CONF_NAME) or CORE.friendly_name or CORE.name
    cg.add(var.set_advertised_name(name))
    cg.add(var.set_output_delay_us(config[CONF_OUTPUT_DELAY].total_microseconds))

    # The AirPlay protocol stack: vendored airplay-esp32 + C glue, built as a
    # local ESP-IDF component (airplay_core/CMakeLists.txt). ESPHome writes it
    # into the project's idf_component.yml as a `path:` dependency and adds it
    # to the main component's REQUIRES, so airplay_core.h is on the include path.
    esp32.add_idf_component(name="airplay_core", path=str(AIRPLAY_CORE_DIR))

    # Kconfig symbols declared in airplay_core/Kconfig (same names as upstream).
    esp32.add_idf_sdkconfig_option(
        "CONFIG_AIRPLAY_FORCE_V1", config[CONF_AIRPLAY_1_ONLY]
    )
    esp32.add_idf_sdkconfig_option(
        "CONFIG_AIRPLAY_TIMING_THRESHOLD_MS",
        config[CONF_TIMING_THRESHOLD].total_milliseconds,
    )
    esp32.add_idf_sdkconfig_option(
        "CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS",
        config[CONF_REALTIME_TIMING_THRESHOLD].total_milliseconds,
    )
    esp32.add_idf_sdkconfig_option(
        "CONFIG_ENABLE_AIRPLAY_ARTWORK", config[CONF_ARTWORK]
    )

    # ESPHome excludes these IDF components by default (DEFAULT_EXCLUDED_IDF_COMPONENTS);
    # dacp_client.c needs the HTTP client.
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")

    cg.add_define("USE_AIRPLAY")
