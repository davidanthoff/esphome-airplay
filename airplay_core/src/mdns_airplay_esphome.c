/*
 * mdns_airplay_esphome.c — replacement for upstream
 * main/network/mdns_airplay.c (airplay-esp32 @ 811d5f8, whose licence
 * applies to this derived file; see airplay_core/UPSTREAM.md).
 *
 * Differences from upstream:
 *   - no mdns_init() / mdns_hostname_set(): ESPHome's mdns component has
 *     already done both, and a second mdns_init() returns
 *     ESP_ERR_INVALID_STATE, which upstream ESP_ERROR_CHECKs (= reboot loop).
 *   - the device id comes from airplay_core_read_mac() instead of upstream's
 *     wifi.c helper, which we do not compile. Upstream asks for the WiFi STA
 *     MAC everywhere (rtsp_handlers.c, hap_pair_verify.c), even on Ethernet;
 *     in our build those calls return the identity MAC chosen by the host
 *     (device_mac.c), and this file must use the same one.
 *
 * The TXT records must stay in sync with upstream's (features, pk, flags,
 * ...), so when you bump the vendored upstream, diff its mdns_airplay.c
 * against this file.
 */

#include "mdns_airplay.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

#include "airplay_core_internal.h"
#include "hap.h"
#include "rtsp_handlers.h" /* AIRPLAY_FEATURES_LO / _HI */
#include "settings.h"

static const char *TAG = "mdns_airplay";

#ifdef CONFIG_AIRPLAY_FORCE_V1
#define AIRPLAY_PROTOCOL_VERSION "1"
#else
#define AIRPLAY_PROTOCOL_VERSION "2"
#endif
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#define AIRPLAY_FLAGS "0x4" /* audio receiver */

#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
#define AIRPLAY_METADATA_TYPES "0,1,2"
#else
#define AIRPLAY_METADATA_TYPES "0,2"
#endif

/* Model: configurable (airplay_core_get_model(), default "esphome-airplay").
 * Upstream advertises "AudioAccessory5,1", the HomePod mini, for the speaker
 * icon in iOS. Music Assistant maps that to "Apple / HomePod Mini", registers
 * the receiver as a native Apple player and then sends ALL volume changes to
 * AirPlay, even while Sendspin plays. shairport-sync advertises its own model
 * ("ShairportSync") and iOS treats it as a normal AirPlay 2 speaker.
 *
 * Manufacturer: not something iOS needs, but Music Assistant uses
 * manufacturer + model verbatim when both are present, and only treats
 * "Apple" + HomePod/Apple TV as a native Apple device. So this keeps Music
 * Assistant treating AirPlay as one protocol next to Sendspin even if the
 * model is set back to "AudioAccessory5,1". */
#define AIRPLAY_MANUFACTURER "ESPHome"

void mdns_airplay_init(void) {
  char device_id[18];
  char features_str[32];
  char service_name[80];
  char pk_str[65];
  char device_name[65];

  settings_get_device_name(device_name, sizeof(device_name));

  uint8_t mac[6];
  airplay_core_read_mac(mac, ESP_MAC_WIFI_STA); /* identity MAC, see device_mac.c */
  snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(pk_str + (size_t)i * 2, 3, "%02x", pk[i]);
  }

  snprintf(features_str, sizeof(features_str), "0x%X,0x%X", AIRPLAY_FEATURES_LO,
           AIRPLAY_FEATURES_HI);

  /* RAOP instance name: <MAC>@<name> */
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

#ifndef CONFIG_AIRPLAY_FORCE_V1
  mdns_txt_item_t airplay_txt[] = {
      {"deviceid", device_id},
      {"features", features_str},
      {"flags", AIRPLAY_FLAGS},
      {"model", airplay_core_get_model()},
      {"manufacturer", AIRPLAY_MANUFACTURER},
      {"pk", pk_str},
      {"pi", "00000000-0000-0000-0000-000000000000"},
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
      {"acl", "0"},
  };
  esp_err_t err =
      mdns_service_add(device_name, "_airplay", "_tcp", 7000, airplay_txt,
                       sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _airplay._tcp: %s", esp_err_to_name(err));
  }
#endif

#ifdef CONFIG_AIRPLAY_FORCE_V1
  mdns_txt_item_t raop_txt[] = {
      {"am", airplay_core_get_model()},
      {"manufacturer", AIRPLAY_MANUFACTURER},
      {"tp", "UDP"},         {"sm", "false"},
      {"sv", "false"},       {"ek", "1"},         {"et", "0,1"},
      {"md", AIRPLAY_METADATA_TYPES},             {"cn", "0,1"},
      {"ch", "2"},           {"ss", "16"},        {"sr", "44100"},
      {"vn", "3"},           {"txtvers", "1"},
  };
#else
  mdns_txt_item_t raop_txt[] = {
      {"am", airplay_core_get_model()},
      {"manufacturer", AIRPLAY_MANUFACTURER},
      {"cn", "0,1,2,3"},
      {"da", "true"},
      {"ek", "1"},
      {"et", "0,1,3,5"},
      {"ft", features_str},
      {"md", AIRPLAY_METADATA_TYPES},
      {"pk", pk_str},
      {"sf", AIRPLAY_FLAGS},
      {"tp", "UDP"},
      {"vn", "65537"},
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
  };
#endif
  esp_err_t err_raop =
      mdns_service_add(service_name, "_raop", "_tcp", 7000, raop_txt,
                       sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp: %s", esp_err_to_name(err_raop));
  }

  ESP_LOGI(TAG, "Advertising AirPlay receiver '%s' (%s)", device_name,
           device_id);
}
