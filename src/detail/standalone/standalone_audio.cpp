#include "standalone_audio.h"
#include "standalone_midi.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace freeaudio::clap_wrapper::standalone
{
const std::vector<uint32_t> &standardSampleRates()
{
  static const std::vector<uint32_t> rates{4000,  5512,  8000,  9600,  11025,  16000,  22050,  32000,
                                           44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
  return rates;
}

std::vector<uint32_t> sampleRatesFrom(const std::vector<std::pair<double, double>> &ranges)
{
  std::vector<uint32_t> res;
  for (const auto &[low, high] : ranges)
  {
    if (high < low || high <= 0) continue;

    if (high - low < 1.0)
    {
      res.push_back(static_cast<uint32_t>(std::lround(low)));
      continue;
    }

    for (auto rate : standardSampleRates())
    {
      if (rate >= low && rate <= high) res.push_back(rate);
    }
  }

  std::sort(res.begin(), res.end());
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}

std::string channelName(const AudioDevice &device, bool input, uint32_t channel)
{
  const auto &names = input ? device.inputChannelNames : device.outputChannelNames;
  if (channel < names.size() && !names[channel].empty()) return names[channel];
  return (input ? "Input " : "Output ") + std::to_string(channel + 1);
}

const AudioDevice *findDevice(const std::vector<AudioDevice> &devices, const std::string &reference,
                              bool input)
{
  if (reference.empty()) return nullptr;

  auto hasDirection = [input](const AudioDevice &d)
  { return input ? d.inputChannels > 0 : d.outputChannels > 0; };

  for (auto field : {&AudioDevice::id, &AudioDevice::legacyName, &AudioDevice::name})
  {
    for (const auto &device : devices)
    {
      if (hasDirection(device) && device.*field == reference) return &device;
    }
  }
  return nullptr;
}

std::vector<uint32_t> commonSampleRates(const AudioDevice *output, const AudioDevice *input)
{
  if (!output && !input) return {};
  if (!output) return input->sampleRates;
  if (!input) return output->sampleRates;

  std::vector<uint32_t> res;
  for (auto rate : output->sampleRates)
  {
    if (std::find(input->sampleRates.begin(), input->sampleRates.end(), rate) !=
        input->sampleRates.end())
      res.push_back(rate);
  }
  return res;
}

std::vector<std::string> deviceMenuTitles(const std::vector<const AudioDevice *> &devices)
{
  std::vector<std::string> res;
  std::map<std::string, int> seen;
  for (const auto *device : devices)
  {
    const auto &name = device->name.empty() ? device->id : device->name;
    const auto count = ++seen[name];
    res.push_back(count == 1 ? name : name + " (" + std::to_string(count) + ")");
  }
  return res;
}

std::string preferredFirstRunInput(const std::vector<AudioDevice> &devices,
                                   const std::string &defaultInput)
{
  const auto *current = findDevice(devices, defaultInput, true);
  if (!current || !current->isMicrophone) return {};

  // the interface with the most inputs: a usb microphone has one, an interface two or more
  const AudioDevice *best{nullptr};
  for (const auto &device : devices)
  {
    if (!device.isInterface || device.isMicrophone || device.inputChannels == 0) continue;
    if (!best || device.inputChannels > best->inputChannels) best = &device;
  }
  return best ? best->id : std::string{};
}

const MidiPort *findMidiPort(const std::vector<MidiPort> &ports, const std::string &reference)
{
  if (reference.empty()) return nullptr;

  for (auto field : {&MidiPort::id, &MidiPort::legacyName, &MidiPort::name})
  {
    for (const auto &port : ports)
    {
      if (port.*field == reference) return &port;
    }
  }
  return nullptr;
}

}  // namespace freeaudio::clap_wrapper::standalone
