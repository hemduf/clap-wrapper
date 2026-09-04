#pragma once

#include <clap/clap.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAP_WRAPPER_EXT_STANDALONE_DEVICE_CONTROL \
  "com.free-audio.clap-wrapper.standalone-device-control/1"

#define CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY 256
#define CLAP_WRAPPER_STANDALONE_API_NAME_CAPACITY 64
#define CLAP_WRAPPER_STANDALONE_API_DISPLAY_NAME_CAPACITY 128

typedef enum clap_wrapper_standalone_device_kind
{
  CLAP_WRAPPER_STANDALONE_AUDIO_INPUT = 0,
  CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT = 1,
  CLAP_WRAPPER_STANDALONE_MIDI_INPUT = 2,
  CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT = 3
} clap_wrapper_standalone_device_kind;

typedef struct clap_wrapper_standalone_audio_api_info
{
  int32_t id;
  char name[CLAP_WRAPPER_STANDALONE_API_NAME_CAPACITY];
  char display_name[CLAP_WRAPPER_STANDALONE_API_DISPLAY_NAME_CAPACITY];
  bool selected;
} clap_wrapper_standalone_audio_api_info;

typedef struct clap_wrapper_standalone_device_info
{
  uint32_t id;
  char name[CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY];
  uint32_t input_channels;
  uint32_t output_channels;
  bool is_default;
  bool selected;
} clap_wrapper_standalone_device_info;

typedef struct clap_wrapper_standalone_audio_configuration
{
  uint32_t input_device_id;
  uint32_t output_device_id;
  bool input_enabled;
  bool output_enabled;
  bool plugin_has_input;
  bool plugin_has_output;
  uint32_t input_channels;
  uint32_t output_channels;
  uint32_t sample_rate;
  uint32_t buffer_size;
} clap_wrapper_standalone_audio_configuration;

/*
 * Standalone-only host extension used by plug-in GUIs that want to expose the
 * host's Audio/MIDI settings. All callbacks are main/UI-thread operations.
 * The extension is intentionally absent when the CLAP is hosted through a DAW
 * or one of clap-wrapper's non-standalone projections.
 */
typedef struct clap_wrapper_host_standalone_device_control
{
  uint32_t(CLAP_ABI *audio_api_count)(const clap_host_t *host);
  bool(CLAP_ABI *audio_api_info)(const clap_host_t *host,
                                 uint32_t index,
                                 clap_wrapper_standalone_audio_api_info *info);
  bool(CLAP_ABI *set_audio_api)(const clap_host_t *host, int32_t api_id);

  uint32_t(CLAP_ABI *device_count)(const clap_host_t *host,
                                   clap_wrapper_standalone_device_kind kind);
  bool(CLAP_ABI *device_info)(const clap_host_t *host,
                              clap_wrapper_standalone_device_kind kind,
                              uint32_t index,
                              clap_wrapper_standalone_device_info *info);

  bool(CLAP_ABI *get_audio_configuration)(
      const clap_host_t *host,
      clap_wrapper_standalone_audio_configuration *configuration);
  bool(CLAP_ABI *set_audio_configuration)(
      const clap_host_t *host,
      const clap_wrapper_standalone_audio_configuration *configuration);

  uint32_t(CLAP_ABI *sample_rate_count)(const clap_host_t *host);
  bool(CLAP_ABI *sample_rate)(const clap_host_t *host, uint32_t index, uint32_t *sample_rate);
  uint32_t(CLAP_ABI *buffer_size_count)(const clap_host_t *host);
  bool(CLAP_ABI *buffer_size)(const clap_host_t *host, uint32_t index, uint32_t *buffer_size);

  bool(CLAP_ABI *set_midi_device_enabled)(const clap_host_t *host,
                                          clap_wrapper_standalone_device_kind kind,
                                          uint32_t device_id,
                                          bool enabled);
  bool(CLAP_ABI *refresh_midi_devices)(const clap_host_t *host);
} clap_wrapper_host_standalone_device_control;

#ifdef __cplusplus
}
#endif
