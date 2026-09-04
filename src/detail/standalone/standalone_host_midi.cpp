#include "standalone_host.h"
#include "standalone_details.h"

#include <algorithm>
#include <chrono>
#include <mutex>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
std::mutex midiInputPushMutex;

bool containsPort(const std::vector<uint32_t> &ports, uint32_t port)
{
  return std::find(ports.begin(), ports.end(), port) != ports.end();
}

size_t midiMessageLength(unsigned char status)
{
  if (status < 0x80) return 3;
  if (status < 0xF0)
  {
    const auto command = status & 0xF0;
    return (command == 0xC0 || command == 0xD0) ? 2 : 3;
  }

  switch (status)
  {
    case 0xF1:
    case 0xF3:
      return 2;
    case 0xF2:
      return 3;
    case 0xF6:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
    case 0xFF:
      return 1;
    default:
      return 3;
  }
}
}  // namespace

std::vector<std::string> StandaloneHost::getMidiInputPortNames()
{
  std::vector<std::string> result;
  try
  {
    RtMidiIn probe;
    const auto count = probe.getPortCount();
    result.reserve(count);
    for (unsigned int i = 0; i < count; ++i) result.push_back(probe.getPortName(i));
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
  }
  return result;
}

std::vector<std::string> StandaloneHost::getMidiOutputPortNames()
{
  std::vector<std::string> result;
  try
  {
    RtMidiOut probe;
    const auto count = probe.getPortCount();
    result.reserve(count);
    for (unsigned int i = 0; i < count; ++i) result.push_back(probe.getPortName(i));
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
  }
  return result;
}

void StandaloneHost::startMIDIThread()
{
  LOGINFO("Initializing Midi");

  const auto inputNames = getMidiInputPortNames();
  const auto outputNames = getMidiOutputPortNames();
  numMidiInputPorts = static_cast<uint32_t>(inputNames.size());
  numMidiOutputPorts = static_cast<uint32_t>(outputNames.size());

  // Preserve clap-wrapper's historical behavior on first launch: every MIDI
  // input is enabled. MIDI output remains opt-in to avoid duplicating hardware
  // messages unexpectedly when a CLAP happens to emit MIDI.
  if (!midiInputSelectionInitialized)
  {
    currentMidiInputPorts.clear();
    for (uint32_t i = 0; i < numMidiInputPorts; ++i) currentMidiInputPorts.push_back(i);
    midiInputSelectionInitialized = true;
  }
  if (!midiOutputSelectionInitialized)
  {
    currentMidiOutputPorts.clear();
    midiOutputSelectionInitialized = true;
  }

  LOGDETAIL("MIDI: {} input sources available, {} selected.", numMidiInputPorts,
            currentMidiInputPorts.size());
  for (const auto port : currentMidiInputPorts)
  {
    if (port >= numMidiInputPorts) continue;
    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOGDETAIL("  input {}: '{}'", port, inputNames[port]);
      midiIn->openPort(port);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
    }
    catch (RtMidiError &error)
    {
      error.printMessage();
    }
  }

  LOGDETAIL("MIDI: {} output destinations available, {} selected.", numMidiOutputPorts,
            currentMidiOutputPorts.size());
  for (const auto port : currentMidiOutputPorts)
  {
    if (port >= numMidiOutputPorts) continue;
    try
    {
      auto midiOut = std::make_unique<RtMidiOut>();
      LOGDETAIL("  output {}: '{}'", port, outputNames[port]);
      midiOut->openPort(port);
      midiOuts.push_back(std::move(midiOut));
    }
    catch (RtMidiError &error)
    {
      error.printMessage();
    }
  }

  if (!midiOuts.empty())
  {
    midiOutputWorkerRunning = true;
    midiOutputWorker = std::thread([this]() { midiOutputWorkerLoop(); });
  }
}

void StandaloneHost::reopenMIDIPorts()
{
  stopMIDIThread();
  startMIDIThread();
}

bool StandaloneHost::setMIDIPortEnabled(bool input, uint32_t port, bool enabled)
{
  const auto names = input ? getMidiInputPortNames() : getMidiOutputPortNames();
  if (port >= names.size()) return false;

  auto &selected = input ? currentMidiInputPorts : currentMidiOutputPorts;
  auto &initialized = input ? midiInputSelectionInitialized : midiOutputSelectionInitialized;
  initialized = true;

  const auto it = std::find(selected.begin(), selected.end(), port);
  if (enabled && it == selected.end())
    selected.push_back(port);
  else if (!enabled && it != selected.end())
    selected.erase(it);
  else
    return true;

  reopenMIDIPorts();
  return true;
}

void StandaloneHost::processMIDIEvents(double deltatime, std::vector<unsigned char> *message)
{
  if (!message) return;
  const auto nBytes = message->size();

  if (nBytes > 0 && nBytes <= 3)
  {
    midiChunk ck;
    memset(ck.dat, 0, sizeof(ck.dat));
    memcpy(ck.dat, message->data(), nBytes);

    // Multiple RtMidi input callbacks can run on different backend threads.
    // Serialize producers while keeping the audio-thread consumer lock-free.
    std::lock_guard<std::mutex> guard(midiInputPushMutex);
    midiToAudioQueue.push(ck);
  }
}

void StandaloneHost::midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData)
{
  auto sh = (StandaloneHost *)userData;
  sh->processMIDIEvents(deltatime, message);
}

bool StandaloneHost::oe_try_push(const struct clap_output_events *oe, const clap_event_header_t *evt)
{
  if (!oe || !oe->ctx || !evt) return false;
  auto *sh = static_cast<StandaloneHost *>(oe->ctx);

  if (evt->space_id != CLAP_CORE_EVENT_SPACE_ID || evt->type != CLAP_EVENT_MIDI ||
      evt->size < sizeof(clap_event_midi_t))
  {
    // Preserve the old standalone behavior for output event kinds that are not
    // routable to a MIDI 1.0 hardware port.
    return true;
  }

  const auto *midi = reinterpret_cast<const clap_event_midi_t *>(evt);
  midiChunk ck;
  ck.dat[0] = midi->data[0];
  ck.dat[1] = midi->data[1];
  ck.dat[2] = midi->data[2];
  sh->midiFromAudioQueue.push(ck);
  return true;
}

void StandaloneHost::midiOutputWorkerLoop()
{
  using namespace std::chrono_literals;

  while (midiOutputWorkerRunning)
  {
    midiChunk ck;
    bool sentAny = false;
    while (midiFromAudioQueue.pop(ck))
    {
      sentAny = true;
      const auto length = midiMessageLength(ck.dat[0]);
      std::vector<unsigned char> message(ck.dat, ck.dat + length);
      for (auto &midiOut : midiOuts)
      {
        if (!midiOut) continue;
        try
        {
          midiOut->sendMessage(&message);
        }
        catch (RtMidiError &error)
        {
          error.printMessage();
        }
      }
    }

    if (!sentAny) std::this_thread::sleep_for(1ms);
  }
}

void StandaloneHost::stopMIDIThread()
{
  midiOutputWorkerRunning = false;
  if (midiOutputWorker.joinable()) midiOutputWorker.join();

  for (auto &m : midiIns) m.reset();
  midiIns.clear();
  for (auto &m : midiOuts) m.reset();
  midiOuts.clear();
}

}  // namespace freeaudio::clap_wrapper::standalone