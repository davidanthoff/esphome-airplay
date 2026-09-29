/*
 * airplay_core.c — replaces upstream main/main.c for the ESPHome build.
 *
 * Upstream's app_main() also brings up NVS, WiFi/Ethernet, a web UI, OTA,
 * LEDs, buttons, displays and the DAC. ESPHome owns all of that, so this file
 * keeps only the AirPlay bring-up sequence from start_airplay_services() and
 * connects the core to its host through airplay_core_host_t.
 *
 * STATUS: scaffold. Compiles against the upstream APIs as read at commit
 * 811d5f8, but has not been built or run yet. See HANDOFF.md, milestone M1.
 */

#include "airplay_core.h"
#include "airplay_core_internal.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "airplay_sodium.h"

/* upstream headers (vendored under upstream/main) */
#include "audio_output.h"
#include "audio_receiver.h"
#include "dac.h"
#include "dacp_client.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "playback_control.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "settings.h"

static const char *TAG = "airplay_core";

static airplay_core_config_t s_config;
static char s_name[65];
static char s_model[33] = "esphome-airplay";
static bool s_initialized = false;
static bool s_infra_started = false; /* PTP/HAP/receiver: once per boot */
static bool s_running = false;

const char *airplay_core_get_name(void) { return s_name; }
const char *airplay_core_get_model(void) { return s_model; }

size_t airplay_core_host_write(const int16_t *pcm, size_t frames,
                               uint32_t sample_rate, uint32_t timeout_ms) {
  if (s_config.host.write == NULL) {
    return 0;
  }
  return s_config.host.write(s_config.host.ctx, pcm, frames, sample_rate,
                             timeout_ms);
}

/* ---- volume: upstream calls dac_set_volume(dB) on every volume change ---- */

static void dac_set_volume_to_host(float volume_db) {
  if (s_config.host.on_volume != NULL) {
    s_config.host.on_volume(s_config.host.ctx, volume_db);
  }
}

static const dac_ops_t s_host_dac_ops = {
    .set_volume = dac_set_volume_to_host,
    /* init/deinit/power/speaker/line-out stay NULL = no-ops */
};

/* ---- session events --------------------------------------------------- */

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  if (s_config.host.on_event == NULL) {
    return;
  }
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    s_config.host.on_event(s_config.host.ctx,
                           AIRPLAY_CORE_EVENT_CLIENT_CONNECTED, NULL);
    break;
  case RTSP_EVENT_PLAYING:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_PLAYING,
                           NULL);
    break;
  case RTSP_EVENT_PAUSED:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_PAUSED,
                           NULL);
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_DISCONNECTED,
                           NULL);
    break;
  case RTSP_EVENT_METADATA: {
    if (data == NULL) {
      break;
    }
    airplay_core_metadata_t md = {0};
    strncpy(md.title, data->metadata.title, sizeof(md.title) - 1);
    strncpy(md.artist, data->metadata.artist, sizeof(md.artist) - 1);
    strncpy(md.album, data->metadata.album, sizeof(md.album) - 1);
    md.duration_secs = data->metadata.duration_secs;
    md.position_secs = data->metadata.position_secs;
    s_config.host.on_event(s_config.host.ctx, AIRPLAY_CORE_EVENT_METADATA,
                           &md);
    break;
  }
  default:
    break;
  }
}

/* ---- public API -------------------------------------------------------- */

esp_err_t airplay_core_init(const airplay_core_config_t *config) {
  if (s_initialized) {
    return ESP_OK;
  }
  ESP_RETURN_ON_FALSE(config != NULL && config->name != NULL,
                      ESP_ERR_INVALID_ARG, TAG, "config and name required");

  s_config = *config;
  strncpy(s_name, config->name, sizeof(s_name) - 1);
  s_config.name = s_name;
  if (config->model != NULL && config->model[0] != '\0') {
    strncpy(s_model, config->model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
  }
  s_config.model = s_model;

  /* NVS is already initialised by ESPHome. Upstream keeps its own data
   * (pairing keys, volume, name) in the "airplay" NVS namespace, which does
   * not collide with ESPHome's. */
  /* Before anything upstream can ask for the device id (settings, HAP, mDNS). */
  airplay_core_set_device_mac(config->device_mac);

  /* Before hap_init() calls sodium_init(): ESPHome's libsodium port would
   * otherwise read /dev/urandom, which ESP-IDF lacks, and abort the firmware
   * (see airplay_sodium/randombytes_esp32.c). */
  airplay_sodium_use_esp32_random();

  ESP_RETURN_ON_ERROR(settings_init(), TAG, "settings_init failed");

  /* rtsp_handlers.c (/info) and mdns read the name from settings. Only write
   * when it changed, to avoid a flash write on every boot. */
  char stored[sizeof(s_name)] = {0};
  if (settings_get_device_name(stored, sizeof(stored)) != ESP_OK ||
      strcmp(stored, s_name) != 0) {
    settings_set_device_name(s_name);
  }

  ESP_RETURN_ON_ERROR(playback_control_init(), TAG,
                      "playback_control_init failed");

  dac_register(&s_host_dac_ops);
  dac_init(NULL);

  airplay_output_configure(config->output_delay_us);

  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGE(TAG, "rtsp_events_register failed (listener table full)");
    return ESP_FAIL;
  }

  s_initialized = true;
  return ESP_OK;
}

esp_err_t airplay_core_start(void) {
  ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "airplay_core_init() first");
  if (s_running) {
    return ESP_OK;
  }

  /* Mirrors upstream start_airplay_services() in main/main.c. */
  if (!s_infra_started) {
    ESP_RETURN_ON_ERROR(ptp_clock_init(), TAG, "ptp_clock_init failed");
    ESP_RETURN_ON_ERROR(hap_init(), TAG, "hap_init failed");
    ESP_RETURN_ON_ERROR(audio_receiver_init(), TAG,
                        "audio_receiver_init failed");
    ESP_RETURN_ON_ERROR(audio_output_init(), TAG, "audio_output_init failed");
    mdns_airplay_init(); /* our version: see mdns_airplay_esphome.c */
    s_infra_started = true;
  }

  audio_output_start();
  ESP_RETURN_ON_ERROR(rtsp_server_start(), TAG, "rtsp_server_start failed");
  s_running = true;
  /* As upstream: route playback_control's play/pause/volume to AirPlay. */
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay receiver '%s' started", s_name);
  return ESP_OK;
}

void airplay_core_stop(void) {
  if (!s_running) {
    return;
  }
  rtsp_server_stop();
  audio_output_stop();
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  s_running = false;
}

void airplay_core_set_output_active(bool active) {
  airplay_output_set_active(active);
}

void airplay_core_reset_output_cursor(void) { airplay_output_reset_cursor(); }

void airplay_core_notify_played(uint32_t frames, int64_t timestamp_us) {
  airplay_output_notify_played(frames, timestamp_us);
}

void airplay_core_disconnect_client(void) {
  /* TODO(M3): upstream has no public "drop the current client" call; the
   * client slots are static in rtsp_server.c. Restarting the server works
   * but also closes the listening socket for a moment. Consider adding a
   * small rtsp_server_disconnect_clients() upstream (it would reuse
   * signal_old_client_stop()). */
  if (!s_running) {
    return;
  }
  rtsp_server_stop();
  if (rtsp_server_start() != ESP_OK) {
    ESP_LOGE(TAG, "rtsp_server_start failed after disconnect");
  }
}

bool airplay_core_remote_command(airplay_core_remote_t command) {
  if (!dacp_is_active()) {
    return false; /* AirPlay 2 session: no DACP, and MRP is not implemented */
  }
  switch (command) {
  case AIRPLAY_CORE_REMOTE_PLAY_PAUSE:
    dacp_send_playpause();
    return true;
  case AIRPLAY_CORE_REMOTE_NEXT:
    dacp_send_next();
    return true;
  case AIRPLAY_CORE_REMOTE_PREVIOUS:
    dacp_send_prev();
    return true;
  default:
    return false;
  }
}
