#pragma once
#include <memory>
#include <optional>
namespace Screen {
class AudioVolume {
public:
    AudioVolume();
    ~AudioVolume();
    std::optional<double> sample();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Screen
