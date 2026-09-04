#include "standalone_host.h"
#include "entry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
StandaloneHost *currentHost()
{
  return getStandaloneHost();
}

template <size_t N>
void copyText(char (&destination)[N], const std::string &source)
{
  std::snprintf(destination, N, "%s", source.c_str());
}

bool contains(const std::vector<uint32_t> &values, uint32_t value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<uint32_t> sampleRatesForDevice(const std::vector<RtAudio::DeviceInfo> &devices,
                                           uint32_t id)
{
  const auto it = std::find_if(devices.begin(), devices.end(),
                               [id](const auto &device) { return device.ID == id; });
  if (it == devices.end()) return {};
  return std::vector<uint32_t>(it->sampleRates.begin(), it->sampleRates.end());
}

std::vector<uint32_t> availableSampleRates(StandaloneHost &host)
{
  try
  {
    host.guaranteeRtAudioDAC();

    const auto inputs = host.getInputAudioDevices();
    const auto outputs = host.getOutputAudioDevices();
    const auto inputRates = host.audioInputUsed
                                ? sampleRatesForDevice(inputs, host.audioInputDeviceID)
                                : std::vector<uint32_t>{};
    const auto outputRates = host.audioOutputUsed
                                 ? sampleRatesForDevice(outputs, host.audioOutputDeviceID)
                                 : std::vector<uint32_t>{};

    if (!inputRates.empty() && !outputRates.empty())
    {
      std::vector<uint32_t> intersection;
      for (const auto rate : outputRates)
        if (contains(inputRates, rate)) intersection.push_back(rate);
      return intersection;
    }
    if (!outputRates.empty()) return outputRates;
    if (!inputRates.empty()) return inputRates;

    // If a selected device disappeared, do not dereference its stale RtAudio
    // ID. Offer the first currently available device's rates so the UI can
    // recover by selecting a valid endpoint.
    if (!outputs.empty())
      return std::vector<uint32_t>(outputs.front().sampleRates.begin(), outputs.front().sampleRates.end());
    if (!inputs.empty())
      return std::vector<uint32_t>(inputs.front().sampleRates.begin(), inputs.front().sampleRates.end());
  }
  catch (...)
  {
    LOGINFO("[WARNING] Unable to enumerate sample rates after an audio device change");
  }
  return {};
}

uint32_t CLAP_ABI audioApiCount(const clap_host_t *)
{
  auto *host = currentHost();
  return host ? static_cast<uint32_t>(host->getCompiledApi().size()) : 0;
}

bool CLAP_ABI audioApiInfo(const clap_host_t *, uint32_t index,
                           clap_wrapper_standalone_audio_api_info *info)
{
  auto *host = currentHost();
  if (!host || !info) return false;
  const auto apis = host->getCompiledApi();
  if (index >= apis.size()) return false;

  host->guaranteeRtAudioDAC();
  const auto api = apis[index];
  *info = {};
  info->id = static_cast<int32_t>(api);
  copyText(info->name, RtAudio::getApiName(api));
  copyText(info->display_name, RtAudio::getApiDisplayName(api));
  info->selected = host->rtaDac->getCurrentApi() == api;
  return true;
}

bool CLAP_ABI setAudioApi(const clap_host_t *, int32_t apiId)
{
  auto *host = currentHost();
  if (!host) return false;

  const auto requested = static_cast<RtAudio::Api>(apiId);
  const auto apis = host->getCompiledApi();
  if (std::find(apis.begin(), apis.end(), requested) == apis.end()) return false;

  host->guaranteeRtAudioDAC();
  if (host->rtaDac->getCurrentApi() == requested) return true;

  host->stopAudioThread();
  host->setAudioApi(requested);
  const auto [input, output, sampleRate] = host->getDefaultAudioInOutSampleRate();
  host->running = true;
  host->finishedRunning = false;
  host->startAudioThreadOn(input, host->totalInputChannels,
                           host->numAudioInputs > 0,
                           output, host->totalOutputChannels,
                           host->numAudioOutputs > 0,
                           sampleRate);
  return host->rtaDac && (host->rtaDac->isStreamRunning() || host->numAudioOutputs == 0);
}

uint32_t CLAP_ABI deviceCount(const clap_host_t *, clap_wrapper_standalone_device_kind kind)
{
  auto *host = currentHost();
  if (!host) return 0;

  switch (kind)
  {
    case CLAP_WRAPPER_STANDALONE_AUDIO_INPUT:
      return static_cast<uint32_t>(host->getInputAudioDevices().size());
    case CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT:
      return static_cast<uint32_t>(host->getOutputAudioDevices().size());
    case CLAP_WRAPPER_STANDALONE_MIDI_INPUT:
      return static_cast<uint32_t>(host->getMidiInputPortNames().size());
    case CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT:
      return static_cast<uint32_t>(host->getMidiOutputPortNames().size());
  }
  return 0;
}

bool CLAP_ABI deviceInfo(const clap_host_t *, clap_wrapper_standalone_device_kind kind,
                         uint32_t index, clap_wrapper_standalone_device_info *info)
{
  auto *host = currentHost();
  if (!host || !info) return false;
  *info = {};

  if (kind == CLAP_WRAPPER_STANDALONE_AUDIO_INPUT ||
      kind == CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT)
  {
    const bool input = kind == CLAP_WRAPPER_STANDALONE_AUDIO_INPUT;
    const auto devices = input ? host->getInputAudioDevices() : host->getOutputAudioDevices();
    if (index >= devices.size()) return false;
    const auto &device = devices[index];
    info->id = device.ID;
    copyText(info->name, device.name);
    info->input_channels = device.inputChannels;
    info->output_channels = device.outputChannels;
    info->is_default = input ? device.isDefaultInput : device.isDefaultOutput;
    info->selected = input ? (host->audioInputUsed && host->audioInputDeviceID == device.ID)
                           : (host->audioOutputUsed && host->audioOutputDeviceID == device.ID);
    return true;
  }

  const bool input = kind == CLAP_WRAPPER_STANDALONE_MIDI_INPUT;
  if (!input && kind != CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT) return false;
  const auto names = input ? host->getMidiInputPortNames() : host->getMidiOutputPortNames();
  if (index >= names.size()) return false;
  info->id = index;
  copyText(info->name, names[index]);
  const auto &selected = input ? host->currentMidiInputPorts : host->currentMidiOutputPorts;
  info->selected = contains(selected, index);
  return true;
}

bool CLAP_ABI getAudioConfiguration(const clap_host_t *,
                                    clap_wrapper_standalone_audio_configuration *configuration)
{
  auto *host = currentHost();
  if (!host || !configuration) return false;
  *configuration = {};
  configuration->input_device_id = host->audioInputDeviceID;
  configuration->output_device_id = host->audioOutputDeviceID;
  configuration->input_enabled = host->audioInputUsed;
  configuration->output_enabled = host->audioOutputUsed;
  configuration->plugin_has_input = host->numAudioInputs > 0;
  configuration->plugin_has_output = host->numAudioOutputs > 0;
  configuration->input_channels = host->currentInputChannels;
  configuration->output_channels = host->currentOutputChannels;
  configuration->sample_rate = host->currentSampleRate > 0
                                   ? static_cast<uint32_t>(host->currentSampleRate)
                                   : 0;
  configuration->buffer_size = host->currentBufferSize;
  return true;
}

bool hasAudioDevice(const std::vector<RtAudio::DeviceInfo> &devices, uint32_t id)
{
  return std::any_of(devices.begin(), devices.end(),
                     [id](const auto &device) { return device.ID == id; });
}

bool CLAP_ABI setAudioConfiguration(
    const clap_host_t *, const clap_wrapper_standalone_audio_configuration *configuration)
{
  auto *host = currentHost();
  if (!host || !configuration) return false;

  host->guaranteeRtAudioDAC();
  const bool useInput = configuration->input_enabled && host->numAudioInputs > 0;
  const bool useOutput = configuration->output_enabled && host->numAudioOutputs > 0;
  const auto inputs = host->getInputAudioDevices();
  const auto outputs = host->getOutputAudioDevices();

  if (useInput && !hasAudioDevice(inputs, configuration->input_device_id)) return false;
  if (useOutput && !hasAudioDevice(outputs, configuration->output_device_id)) return false;

  if (!useInput && !useOutput)
  {
    host->stopAudioThread();
    host->audioInputUsed = false;
    host->audioOutputUsed = false;
    host->currentInputChannels = 0;
    host->currentOutputChannels = 0;
    return true;
  }

  if (configuration->buffer_size > 0)
    host->currentBufferSize = configuration->buffer_size;

  const auto sampleRate = configuration->sample_rate > 0
                              ? static_cast<int32_t>(configuration->sample_rate)
                              : -1;

  host->running = true;
  host->finishedRunning = false;
  host->startAudioThreadOn(configuration->input_device_id,
                           host->totalInputChannels,
                           useInput,
                           configuration->output_device_id,
                           host->totalOutputChannels,
                           useOutput,
                           sampleRate);
  return host->rtaDac && host->rtaDac->isStreamRunning();
}

uint32_t CLAP_ABI sampleRateCount(const clap_host_t *)
{
  auto *host = currentHost();
  return host ? static_cast<uint32_t>(availableSampleRates(*host).size()) : 0;
}

bool CLAP_ABI sampleRate(const clap_host_t *, uint32_t index, uint32_t *result)
{
  auto *host = currentHost();
  if (!host || !result) return false;
  const auto rates = availableSampleRates(*host);
  if (index >= rates.size()) return false;
  *result = rates[index];
  return true;
}

uint32_t CLAP_ABI bufferSizeCount(const clap_host_t *)
{
  auto *host = currentHost();
  return host ? static_cast<uint32_t>(host->getBufferSizes().size()) : 0;
}

bool CLAP_ABI bufferSize(const clap_host_t *, uint32_t index, uint32_t *result)
{
  auto *host = currentHost();
  if (!host || !result) return false;
  const auto sizes = host->getBufferSizes();
  if (index >= sizes.size()) return false;
  *result = sizes[index];
  return true;
}

bool CLAP_ABI setMidiDeviceEnabled(const clap_host_t *, clap_wrapper_standalone_device_kind kind,
                                   uint32_t deviceId, bool enabled)
{
  auto *host = currentHost();
  if (!host) return false;
  if (kind == CLAP_WRAPPER_STANDALONE_MIDI_INPUT)
    return host->setMIDIPortEnabled(true, deviceId, enabled);
  if (kind == CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT)
    return host->setMIDIPortEnabled(false, deviceId, enabled);
  return false;
}

bool CLAP_ABI refreshMidiDevices(const clap_host_t *)
{
  auto *host = currentHost();
  if (!host) return false;
  host->reopenMIDIPorts();
  return true;
}

const clap_wrapper_host_standalone_device_control standaloneDeviceControl{
    audioApiCount,
    audioApiInfo,
    setAudioApi,
    deviceCount,
    deviceInfo,
    getAudioConfiguration,
    setAudioConfiguration,
    sampleRateCount,
    sampleRate,
    bufferSizeCount,
    bufferSize,
    setMidiDeviceEnabled,
    refreshMidiDevices};
}  // namespace

const void *StandaloneHost::getHostExtension(const char *extension)
{
  if (extension && std::strcmp(extension, CLAP_WRAPPER_EXT_STANDALONE_DEVICE_CONTROL) == 0)
    return &standaloneDeviceControl;
  return nullptr;
}

}  // namespace freeaudio::clap_wrapper::standalone
