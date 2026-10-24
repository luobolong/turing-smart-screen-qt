#include "audio.h"
#include <QtGlobal>
#ifdef Q_OS_LINUX
#include <pulse/pulseaudio.h>
#endif
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif
namespace Screen {
struct AudioVolume::Impl {
#ifdef Q_OS_LINUX
    pa_threaded_mainloop *loop = nullptr;
    pa_context *context = nullptr;
    std::optional<double> level;
    bool started = false, pending = false, refreshAgain = false;

    static void release(pa_operation *operation) {
        if (operation)
            pa_operation_unref(operation);
    }
    void finish() {
        pending = false;
        if (refreshAgain) {
            refreshAgain = false;
            refresh();
        }
    }
    void refresh() {
        if (!context || pa_context_get_state(context) != PA_CONTEXT_READY)
            return;
        if (pending) {
            refreshAgain = true;
            return;
        }
        pending = true;
        auto operation = pa_context_get_server_info(context, [](pa_context *c, const pa_server_info *info,
                                                                void *userdata) {
            auto self = static_cast<Impl *>(userdata);
            if (!info || !info->default_sink_name) {
                self->level.reset();
                self->finish();
                return;
            }
            auto sink = pa_context_get_sink_info_by_name(c, info->default_sink_name,
                [](pa_context *, const pa_sink_info *info, int eol, void *userdata) {
                    auto self = static_cast<Impl *>(userdata);
                    if (eol) {
                        if (eol < 0)
                            self->level.reset();
                        self->finish();
                    } else if (info && pa_cvolume_valid(&info->volume)) {
                        self->level = info->mute ? 0. : 100. * pa_cvolume_avg(&info->volume) / PA_VOLUME_NORM;
                    } else {
                        self->level.reset();
                    }
                }, self);
            if (!sink) {
                self->level.reset();
                self->finish();
            }
            release(sink);
        }, this);
        if (!operation) {
            level.reset();
            pending = false;
        }
        release(operation);
    }
    void connect() {
        level.reset();
        pending = refreshAgain = false;
        if (context) {
            pa_context_set_state_callback(context, nullptr, nullptr);
            pa_context_set_subscribe_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(context);
        }
        context = pa_context_new(pa_threaded_mainloop_get_api(loop), "Turing Screen");
        if (!context)
            return;
        pa_context_set_state_callback(context, [](pa_context *c, void *userdata) {
            auto self = static_cast<Impl *>(userdata);
            if (pa_context_get_state(c) == PA_CONTEXT_READY) {
                pa_context_set_subscribe_callback(c, [](pa_context *, pa_subscription_event_type_t,
                                                       uint32_t, void *userdata) {
                    auto self = static_cast<Impl *>(userdata);
                    self->level.reset();
                    self->refresh();
                }, self);
                release(pa_context_subscribe(c, pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK |
                                                                       PA_SUBSCRIPTION_MASK_SERVER),
                                             nullptr, nullptr));
                self->refresh();
            } else if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(c))) {
                self->level.reset();
                self->pending = self->refreshAgain = false;
            }
        }, this);
        pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
    }
    Impl() {
        loop = pa_threaded_mainloop_new();
        if (loop) {
            connect();
            started = pa_threaded_mainloop_start(loop) == 0;
        }
    }
    ~Impl() {
        if (started)
            pa_threaded_mainloop_stop(loop);
        if (context) {
            pa_context_set_state_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(context);
        }
        if (loop)
            pa_threaded_mainloop_free(loop);
    }
    std::optional<double> sample() {
        if (!started)
            return {};
        pa_threaded_mainloop_lock(loop);
        if (!context || !PA_CONTEXT_IS_GOOD(pa_context_get_state(context)))
            connect();
        else
            refresh();
        const auto result = level;
        pa_threaded_mainloop_unlock(loop);
        return result;
    }
#elif defined(Q_OS_WIN)
    std::optional<double> sample() {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            return {};
        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        IAudioEndpointVolume *volume = nullptr;
        std::optional<double> result;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void **>(&volume)))) {
            float value = 0;
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&value)) && SUCCEEDED(volume->GetMute(&muted)))
                result = muted ? 0. : value * 100.;
        }
        if (volume)
            volume->Release();
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
        if (SUCCEEDED(initialized))
            CoUninitialize();
        return result;
    }
#else
    std::optional<double> sample() { return {}; }
#endif
};
AudioVolume::AudioVolume() : impl(std::make_unique<Impl>()) {}
AudioVolume::~AudioVolume() = default;
std::optional<double> AudioVolume::sample() { return impl->sample(); }
} // namespace Screen
