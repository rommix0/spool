#include <windows.h>
#include <initguid.h>

#include <propsys.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include "audio/wasapi_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "core/sample_convert.h"

namespace sl {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint16_t kMaxChannels = 8;
constexpr REFERENCE_TIME kSharedBuffer = 1000000;  // 100 ms in 100 ns units
constexpr DWORD kWaitMs = 200;
constexpr auto kFirstRetry = std::chrono::milliseconds(1000);
constexpr auto kNextRetry = std::chrono::milliseconds(2000);
constexpr uint32_t kProbeRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

// CoInitializeEx(MTA) for the calling scope; tolerates a thread already in another mode.
class ComInit {
 public:
  ComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr_)) CoUninitialize();  // RPC_E_CHANGED_MODE: not ours to undo
  }
  ComInit(const ComInit&) = delete;
  ComInit& operator=(const ComInit&) = delete;

 private:
  HRESULT hr_;
};

struct CoMemFree {
  void operator()(void* p) const { CoTaskMemFree(p); }
};
using WfxPtr = std::unique_ptr<WAVEFORMATEX, CoMemFree>;

std::wstring hr_text(HRESULT hr) {
  wchar_t b[16];
  swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
  return b;
}

void set_err(std::wstring* err, const wchar_t* what, HRESULT hr) {
  if (err) *err = std::wstring(what) + L" (" + hr_text(hr) + L")";
}

enum class SampleKind { F32, I16, I24, I32 };

struct Layout {
  SampleKind kind = SampleKind::F32;
  uint32_t rate = 0;
  uint16_t channels = 0;
};

bool parse_format(const WAVEFORMATEX* w, Layout* out) {
  if (!w || w->nChannels == 0) return false;
  bool is_float = false, is_pcm = false;
  if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
    const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
    is_float = IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    is_pcm = IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
  } else {
    is_float = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    is_pcm = w->wFormatTag == WAVE_FORMAT_PCM;
  }
  Layout l;
  l.rate = w->nSamplesPerSec;
  l.channels = w->nChannels;
  // 24-in-32 (wValidBitsPerSample 24, container 32) is read as int32, left-justified.
  if (is_float && w->wBitsPerSample == 32) l.kind = SampleKind::F32;
  else if (is_pcm && w->wBitsPerSample == 16) l.kind = SampleKind::I16;
  else if (is_pcm && w->wBitsPerSample == 24) l.kind = SampleKind::I24;
  else if (is_pcm && w->wBitsPerSample == 32) l.kind = SampleKind::I32;
  else return false;
  *out = l;
  return true;
}

DWORD default_mask(uint16_t ch) {
  if (ch == 1) return SPEAKER_FRONT_CENTER;
  if (ch == 2) return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  return 0;
}

DWORD mix_mask(const WAVEFORMATEX* mix) {
  if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
  return default_mask(mix->nChannels);
}

// Exclusive candidates in order of preference: int32, 24-in-32, packed 24, int16.
struct Cand {
  WORD container, valid;
};
constexpr Cand kExclusiveCands[] = {{32, 32}, {32, 24}, {24, 24}, {16, 16}};

WAVEFORMATEXTENSIBLE make_pcm(const Cand& c, uint32_t rate, uint16_t ch, DWORD mask) {
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = ch;
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = c.container;
  x.Format.nBlockAlign = WORD(ch * c.container / 8);
  x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
  x.Format.cbSize = 22;
  x.Samples.wValidBitsPerSample = c.valid;
  x.dwChannelMask = mask;
  x.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  return x;
}

bool pick_exclusive_format(IAudioClient* c, uint32_t rate, uint16_t ch, DWORD mask,
                           WAVEFORMATEXTENSIBLE* out) {
  for (const Cand& cand : kExclusiveCands) {
    WAVEFORMATEXTENSIBLE x = make_pcm(cand, rate, ch, mask);
    if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &x.Format, nullptr) == S_OK) {
      *out = x;
      return true;
    }
  }
  return false;
}

ComPtr<IMMDeviceEnumerator> make_enumerator() {
  ComPtr<IMMDeviceEnumerator> en;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
  return en;
}

HRESULT get_device(IMMDeviceEnumerator* en, const std::wstring& id, ComPtr<IMMDevice>* dev) {
  return id.empty() ? en->GetDefaultAudioEndpoint(eCapture, eConsole, dev->GetAddressOf())
                    : en->GetDevice(id.c_str(), dev->GetAddressOf());
}

std::wstring device_id_of(IMMDevice* dev) {
  LPWSTR raw = nullptr;
  std::wstring id;
  if (SUCCEEDED(dev->GetId(&raw)) && raw) id = raw;
  CoTaskMemFree(raw);
  return id;
}

HRESULT activate_client(IMMDevice* dev, ComPtr<IAudioClient>* c) {
  return dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(c->GetAddressOf()));
}

// The endpoint's System.Devices.AudioDevice.RawProcessingSupported; false if unknown.
bool raw_supported(IMMDevice* dev) {
  ComPtr<IPropertyStore> props;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return false;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  const bool yes = SUCCEEDED(props->GetValue(PKEY_Devices_AudioDevice_RawProcessingSupported, &pv)) &&
                   pv.vt == VT_BOOL && pv.boolVal == VARIANT_TRUE;
  PropVariantClear(&pv);
  return yes;
}

}  // namespace

// ---- free functions ----

bool capture_timed_out(uint64_t last_packet_ms, uint64_t now_ms) {
  return now_ms >= last_packet_ms && now_ms - last_packet_ms >= kCaptureSilenceTimeoutMs;
}

std::vector<DeviceInfo> list_capture_devices() {
  std::vector<DeviceInfo> out;
  try {
    ComInit com;
    auto en = make_enumerator();
    if (!en) return out;
    ComPtr<IMMDeviceCollection> col;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) ||
        FAILED(col->GetCount(&n)))
      return out;
    for (UINT i = 0; i < n; ++i) {
      ComPtr<IMMDevice> dev;
      if (FAILED(col->Item(i, &dev))) continue;
      DeviceInfo d;
      d.id = device_id_of(dev.Get());
      if (d.id.empty()) continue;
      ComPtr<IPropertyStore> props;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
          d.name = pv.pwszVal;
        PropVariantClear(&pv);
      }
      if (d.name.empty()) d.name = d.id;
      ComPtr<IAudioClient> client;
      if (SUCCEEDED(activate_client(dev.Get(), &client))) {
        WAVEFORMATEX* raw = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&raw))) {
          WfxPtr mix(raw);
          d.mix_format = {mix->nSamplesPerSec, mix->nChannels};
        }
      }
      out.push_back(std::move(d));
    }
  } catch (...) {
    out.clear();
  }
  return out;
}

std::wstring default_capture_device_id() {
  try {
    ComInit com;
    auto en = make_enumerator();
    ComPtr<IMMDevice> dev;
    if (en && SUCCEEDED(get_device(en.Get(), L"", &dev))) return device_id_of(dev.Get());
  } catch (...) {
  }
  return {};
}

std::vector<uint32_t> exclusive_rates(const std::wstring& id) {
  std::vector<uint32_t> out;
  try {
    ComInit com;
    auto en = make_enumerator();
    ComPtr<IMMDevice> dev;
    if (!en || FAILED(get_device(en.Get(), id, &dev))) return out;
    ComPtr<IAudioClient> client;
    if (FAILED(activate_client(dev.Get(), &client))) return out;
    WAVEFORMATEX* raw = nullptr;
    if (FAILED(client->GetMixFormat(&raw))) return out;
    WfxPtr mix(raw);
    if (mix->nChannels > kMaxChannels) return out;
    const DWORD mask = mix_mask(mix.get());
    for (uint32_t r : kProbeRates) {
      WAVEFORMATEXTENSIBLE x;
      if (pick_exclusive_format(client.Get(), r, mix->nChannels, mask, &x)) out.push_back(r);
    }
  } catch (...) {
    out.clear();
  }
  return out;
}

// ---- WasapiImpl ----
//
// Threads: the caller's thread only posts jobs; a worker thread (MTA) owns every COM object
// and does open/close/start/stop, device-return reopening and the loss/return callbacks; the
// capture thread (MTA, MMCSS "Pro Audio") only drains the capture client. The IMMNotificationClient
// callback just queues an event for the worker. Nothing calls a callback while holding a lock.

class WasapiImpl {
 public:
  struct Event {
    enum Kind { Gone, Return, CaptureFailed } kind;
    std::wstring id;
    uint64_t gen = 0;
  };

  WasapiImpl() {
    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~WasapiImpl() {
    std::thread w;
    {
      std::lock_guard<std::mutex> lk(wk_m_);
      quit_ = true;
      w = std::move(worker_);
    }
    wk_cv_.notify_all();
    if (w.joinable()) w.join();
    CloseHandle(buffer_event_);
    CloseHandle(stop_event_);
  }

  bool open(const CaptureConfig& cfg, std::wstring* err) {
    if (!ensure_worker(err)) return false;
    bool ok = false;
    run([&] {
      close_client();
      lost_.store(false);
      retry_pending_ = false;
      cfg_ = cfg;
      ok = open_client(cfg_, err);
      opened_ = ok;
      if (ok) {
        cfg_.device_id = dev_id_;  // pin the endpoint: a default-device config never follows another device
      } else {
        std::lock_guard<std::mutex> lk(fmt_m_);
        fmt_ = {};
        pinned_id_.clear();
        raw_active_.store(false);
        exclusive_.store(false);
      }
    });
    return ok;
  }

  StreamFormat format() const {
    std::lock_guard<std::mutex> lk(fmt_m_);
    return fmt_;
  }

  bool start(CaptureCallbacks* cb) {
    if (!cb) return false;
    cb_.store(cb);
    bool ok = false;
    run([&] { ok = start_stream(); });
    return ok;
  }

  void stop() {
    run([&] { stop_capture_thread(); });
  }

  bool raw_active() const { return raw_active_.load(); }
  bool exclusive() const { return exclusive_.load(); }

  // The endpoint id the source is bound to (resolved at open(); empty when not open).
  std::wstring device_id() const {
    std::lock_guard<std::mutex> lk(fmt_m_);
    return pinned_id_;
  }

  // Any thread (also COM notification threads): only a short mutex, never blocks on work.
  void post(Event e) {
    {
      std::lock_guard<std::mutex> lk(wk_m_);
      events_.push_back(std::move(e));
    }
    wk_cv_.notify_one();
  }

 private:
  class NotifClient;

  bool ensure_worker(std::wstring* err) {
    std::promise<HRESULT> ready;
    auto fut = ready.get_future();
    {
      std::lock_guard<std::mutex> lk(wk_m_);
      if (worker_.joinable()) return true;
      if (quit_) return false;
      worker_ = std::thread([this, p = std::move(ready)]() mutable { worker_main(std::move(p)); });
    }
    const HRESULT hr = fut.get();
    if (FAILED(hr)) {
      std::thread w;
      {
        std::lock_guard<std::mutex> lk(wk_m_);
        w = std::move(worker_);
      }
      w.join();
      set_err(err, L"Could not initialize the audio device service", hr);
      return false;
    }
    return true;
  }

  // Runs f on the worker and waits for it. False when there is no worker.
  template <class F>
  bool run(F&& f) {
    if (std::this_thread::get_id() == worker_tid_) {  // e.g. start() from on_device_returned
      f();
      return true;
    }
    auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
    auto done = task->get_future();
    {
      std::lock_guard<std::mutex> lk(wk_m_);
      if (quit_ || !worker_.joinable()) return false;
      jobs_.push_back([task] { (*task)(); });
    }
    wk_cv_.notify_one();
    done.get();
    return true;
  }

  void worker_main(std::promise<HRESULT> ready);
  void handle_event(const Event& e);
  void handle_loss();
  void try_reopen();
  bool matches_device(const std::wstring& id);

  bool open_client(const CaptureConfig& cfg, std::wstring* err);
  bool open_shared(IMMDevice* dev, bool raw, std::wstring* err);
  bool open_exclusive(IMMDevice* dev, uint32_t rate, std::wstring* err);
  bool finish_open(ComPtr<IAudioClient> client, const WAVEFORMATEX* wfx, std::wstring* err);
  void close_client();
  bool start_stream();
  void stop_capture_thread();
  void capture_main(uint64_t gen);

  // Shared with other threads.
  mutable std::mutex fmt_m_;
  StreamFormat fmt_;
  std::wstring pinned_id_;
  std::atomic<bool> raw_active_{false};
  std::atomic<bool> exclusive_{false};
  std::atomic<bool> lost_{false};
  std::atomic<CaptureCallbacks*> cb_{nullptr};
  std::atomic<bool> run_{false};

  std::mutex wk_m_;
  std::condition_variable wk_cv_;
  std::deque<std::function<void()>> jobs_;
  std::deque<Event> events_;
  bool quit_ = false;
  std::thread worker_;
  std::thread::id worker_tid_;  // set by the worker before it reports ready
  // Capture thread -> worker failure signal (gen + 1 of the failed stream); lock-free to set.
  std::atomic<uint64_t> fail_gen_{0};

  HANDLE buffer_event_ = nullptr, stop_event_ = nullptr;

  // Worker only. The capture thread reads the client, layout and buffer while it runs; they
  // change only after it has been joined.
  ComPtr<IMMDeviceEnumerator> en_;
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioCaptureClient> cap_;
  CaptureConfig cfg_;
  std::wstring dev_id_;
  Layout layout_;
  std::vector<float> buf_;
  size_t buf_frames_ = 0;
  std::thread capture_;
  bool opened_ = false;
  uint64_t gen_ = 0;
  bool retry_pending_ = false;
  std::chrono::steady_clock::time_point retry_at_;
};

class WasapiImpl::NotifClient : public IMMNotificationClient {
 public:
  explicit NotifClient(WasapiImpl* p) : p_(p) {}

  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    if (id) p_->post({state == DEVICE_STATE_ACTIVE ? Event::Return : Event::Gone, id, 0});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
    if (id) p_->post({Event::Return, id, 0});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
    if (id) p_->post({Event::Gone, id, 0});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  std::atomic<ULONG> ref_{1};
  WasapiImpl* p_;
};

void WasapiImpl::worker_main(std::promise<HRESULT> ready) {
  ComInit com;
  worker_tid_ = std::this_thread::get_id();
  NotifClient* nc = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en_));
  if (SUCCEEDED(hr)) {
    nc = new NotifClient(this);
    hr = en_->RegisterEndpointNotificationCallback(nc);
  }
  ready.set_value(hr);
  if (FAILED(hr)) {
    if (nc) nc->Release();
    en_.Reset();
    return;
  }

  std::unique_lock<std::mutex> lk(wk_m_);
  while (!quit_) {
    const auto signalled = [&] {
      return quit_ || !jobs_.empty() || !events_.empty() || fail_gen_.load() != 0;
    };
    if (!signalled()) {
      // The capture thread signals without the lock, so a notify can slip between the check
      // and the wait: while it runs, wake every 100 ms to look at fail_gen_ regardless.
      auto until = std::chrono::steady_clock::time_point::max();
      if (retry_pending_) until = retry_at_;
      if (capture_.joinable()) until = std::min(until, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
      if (until == std::chrono::steady_clock::time_point::max()) wk_cv_.wait(lk, signalled);
      else wk_cv_.wait_until(lk, until);
    }
    if (quit_) break;
    auto jobs = std::move(jobs_);
    auto events = std::move(events_);
    jobs_.clear();
    events_.clear();
    lk.unlock();
    for (auto& j : jobs) j();
    for (const Event& e : events) handle_event(e);
    if (const uint64_t f = fail_gen_.exchange(0)) handle_event({Event::CaptureFailed, {}, f - 1});
    if (retry_pending_ && std::chrono::steady_clock::now() >= retry_at_) try_reopen();
    lk.lock();
  }
  lk.unlock();

  close_client();
  en_->UnregisterEndpointNotificationCallback(nc);  // waits for callbacks in flight
  nc->Release();
  en_.Reset();
}

// The source stays bound to the endpoint chosen at open(): a different microphone must
// never resume the session as "the same device".
bool WasapiImpl::matches_device(const std::wstring& id) {
  return id == dev_id_;
}

void WasapiImpl::handle_event(const Event& e) {
  if (!opened_) return;
  switch (e.kind) {
    case Event::Gone:
      if (!lost_.load() && e.id == dev_id_) handle_loss();
      break;
    case Event::CaptureFailed:
      if (e.gen == gen_) handle_loss();  // otherwise from a stream already closed
      break;
    case Event::Return:
      if (lost_.load() && matches_device(e.id)) try_reopen();
      break;
  }
}

// The device is gone (removed, disabled, or the stream was invalidated): stop for good,
// release the client, tell the manager once, and keep trying to bring the device back (a
// format change invalidates the stream without any state notification).
void WasapiImpl::handle_loss() {
  if (lost_.load()) return;
  close_client();  // joins the capture thread: no on_frames after on_device_lost
  lost_.store(true);
  retry_pending_ = true;
  retry_at_ = std::chrono::steady_clock::now() + kFirstRetry;
  if (CaptureCallbacks* cb = cb_.load()) cb->on_device_lost();
}

void WasapiImpl::try_reopen() {
  if (!lost_.load()) {
    retry_pending_ = false;
    return;
  }
  std::wstring err;
  if (!open_client(cfg_, &err)) {
    retry_pending_ = true;
    retry_at_ = std::chrono::steady_clock::now() + kNextRetry;
    return;
  }
  retry_pending_ = false;
  lost_.store(false);
  if (CaptureCallbacks* cb = cb_.load()) cb->on_device_returned(format());
}

// ---- open / close ----

bool WasapiImpl::open_client(const CaptureConfig& cfg, std::wstring* err) {
  close_client();
  ComPtr<IMMDevice> dev;
  const HRESULT hr = get_device(en_.Get(), cfg.device_id, &dev);
  if (FAILED(hr)) {
    set_err(err, cfg.device_id.empty() ? L"No default capture device" : L"Capture device not found", hr);
    return false;
  }
  dev_id_ = device_id_of(dev.Get());
  bool ok;
  if (cfg.mode == CaptureMode::Exclusive) {
    ok = open_exclusive(dev.Get(), cfg.sample_rate, err);
    if (ok) raw_active_.store(true);
  } else {
    // RAW only where the endpoint says it bypasses the processing; otherwise the option may
    // be accepted and silently ignored.
    ok = raw_supported(dev.Get()) && open_shared(dev.Get(), true, err);
    if (!ok) ok = open_shared(dev.Get(), false, err);  // no raw support, or RAW refused
  }
  if (ok) exclusive_.store(cfg.mode == CaptureMode::Exclusive);
  if (!ok) close_client();
  return ok;
}

bool WasapiImpl::open_shared(IMMDevice* dev, bool raw, std::wstring* err) {
  ComPtr<IAudioClient> client;
  HRESULT hr = activate_client(dev, &client);
  if (FAILED(hr)) {
    set_err(err, L"Could not open the capture device", hr);
    return false;
  }
  if (raw) {
    ComPtr<IAudioClient2> c2;
    if (FAILED(client.As(&c2))) return false;
    AudioClientProperties props{};
    props.cbSize = sizeof(props);
    props.eCategory = AudioCategory_Other;
    props.Options = AUDCLNT_STREAMOPTIONS_RAW;
    if (FAILED(c2->SetClientProperties(&props))) return false;
  }
  WAVEFORMATEX* rawfmt = nullptr;
  hr = client->GetMixFormat(&rawfmt);
  if (FAILED(hr)) {
    set_err(err, L"Could not read the device format", hr);
    return false;
  }
  WfxPtr mix(rawfmt);
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kSharedBuffer, 0,
                          mix.get(), nullptr);
  if (FAILED(hr)) {
    set_err(err, raw ? L"Could not start raw capture" : L"Could not start capture", hr);
    return false;
  }
  if (!finish_open(std::move(client), mix.get(), err)) return false;
  raw_active_.store(raw);
  return true;
}

bool WasapiImpl::open_exclusive(IMMDevice* dev, uint32_t rate, std::wstring* err) {
  ComPtr<IAudioClient> client;
  HRESULT hr = activate_client(dev, &client);
  if (FAILED(hr)) {
    set_err(err, L"Could not open the capture device", hr);
    return false;
  }
  WAVEFORMATEX* rawfmt = nullptr;
  hr = client->GetMixFormat(&rawfmt);
  if (FAILED(hr)) {
    set_err(err, L"Could not read the device format", hr);
    return false;
  }
  WfxPtr mix(rawfmt);
  if (mix->nChannels > kMaxChannels) {
    if (err) *err = L"The device has more than 8 channels";
    return false;
  }
  if (rate == 0) rate = mix->nSamplesPerSec;
  WAVEFORMATEXTENSIBLE wfx;
  if (!pick_exclusive_format(client.Get(), rate, mix->nChannels, mix_mask(mix.get()), &wfx)) {
    if (err) *err = L"The device does not support exclusive mode at " + std::to_wstring(rate) + L" Hz";
    return false;
  }
  REFERENCE_TIME def = 0, min_period = 0;
  hr = client->GetDevicePeriod(&def, &min_period);
  if (FAILED(hr)) {
    set_err(err, L"Could not read the device period", hr);
    return false;
  }
  hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, def,
                          &wfx.Format, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    // Retry with the period rounded to the device's buffer alignment.
    UINT32 frames = 0;
    if (FAILED(client->GetBufferSize(&frames)) || frames == 0) {
      set_err(err, L"Could not start exclusive capture", hr);
      return false;
    }
    client.Reset();
    hr = activate_client(dev, &client);
    if (SUCCEEDED(hr)) {
      def = static_cast<REFERENCE_TIME>(10000000.0 / rate * frames + 0.5);
      hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, def,
                              &wfx.Format, nullptr);
    }
  }
  if (FAILED(hr)) {
    set_err(err, L"Could not start exclusive capture", hr);
    return false;
  }
  return finish_open(std::move(client), &wfx.Format, err);
}

bool WasapiImpl::finish_open(ComPtr<IAudioClient> client, const WAVEFORMATEX* wfx, std::wstring* err) {
  Layout l;
  if (!parse_format(wfx, &l)) {
    if (err) *err = L"Unsupported device sample format";
    return false;
  }
  if (l.channels > kMaxChannels) {
    if (err) *err = L"The device has more than 8 channels";
    return false;
  }
  UINT32 frames = 0;
  HRESULT hr = client->GetBufferSize(&frames);
  if (FAILED(hr) || frames == 0) {
    set_err(err, L"Could not read the capture buffer size", hr);
    return false;
  }
  hr = client->SetEventHandle(buffer_event_);
  if (FAILED(hr)) {
    set_err(err, L"Could not set the capture event", hr);
    return false;
  }
  ComPtr<IAudioCaptureClient> cap;
  hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.GetAddressOf()));
  if (FAILED(hr)) {
    set_err(err, L"Could not get the capture service", hr);
    return false;
  }
  try {
    buf_.assign(size_t(frames) * l.channels, 0.0f);  // the only allocation; none while capturing
  } catch (const std::bad_alloc&) {
    if (err) *err = L"Out of memory";
    return false;
  }
  buf_frames_ = frames;
  layout_ = l;
  client_ = std::move(client);
  cap_ = std::move(cap);
  {
    std::lock_guard<std::mutex> lk(fmt_m_);
    fmt_ = {l.rate, l.channels};
    pinned_id_ = dev_id_;
  }
  return true;
}

void WasapiImpl::close_client() {
  stop_capture_thread();
  ++gen_;  // events from the previous stream are stale
  cap_.Reset();
  client_.Reset();
}

// ---- start / stop / capture thread ----

bool WasapiImpl::start_stream() {
  if (!client_ || !cap_ || lost_.load()) return false;
  if (capture_.joinable()) return true;  // already running
  ResetEvent(stop_event_);
  ResetEvent(buffer_event_);
  const HRESULT hr = client_->Start();
  if (FAILED(hr)) {
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) post({Event::CaptureFailed, {}, gen_});
    return false;
  }
  run_.store(true);
  capture_ = std::thread([this, g = gen_] { capture_main(g); });
  return true;
}

void WasapiImpl::stop_capture_thread() {
  run_.store(false);
  SetEvent(stop_event_);
  if (!capture_.joinable()) return;
  capture_.join();
  if (client_) {
    client_->Stop();
    client_->Reset();
  }
}

void WasapiImpl::capture_main(uint64_t gen) {
  ComInit com;
  DWORD task = 0;
  HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  const HANDLE handles[2] = {buffer_event_, stop_event_};
  IAudioCaptureClient* cap = cap_.Get();
  float* const buf = buf_.data();
  const size_t cap_frames = buf_frames_;
  const Layout l = layout_;
  const size_t stride = l.channels;
  bool failed = false;
  // WASAPI flags the first packet after every Start() as a discontinuity; nothing was lost
  // (a device-loss gap is recorded separately), so only later packets are reported.
  bool first_packet = true;
  uint64_t last_packet = GetTickCount64();  // a stream that goes silent without an error is lost

  while (run_.load(std::memory_order_acquire) && !failed) {
    const DWORD w = WaitForMultipleObjects(2, handles, FALSE, kWaitMs);
    if (w == WAIT_OBJECT_0 + 1 || w == WAIT_FAILED) break;
    // Also on timeout: draining doubles as a health probe for a device that went silent.
    for (;;) {
      UINT32 next = 0;
      if (FAILED(cap->GetNextPacketSize(&next))) {
        failed = true;
        break;
      }
      if (next == 0) break;
      BYTE* data = nullptr;
      UINT32 n = 0;
      DWORD flags = 0;
      const HRESULT hr = cap->GetBuffer(&data, &n, &flags, nullptr, nullptr);
      if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
      if (FAILED(hr)) {
        failed = true;
        break;
      }
      last_packet = GetTickCount64();
      CaptureCallbacks* cb = cb_.load(std::memory_order_acquire);
      bool disc = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && !first_packet;
      first_packet = false;
      const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
      for (size_t done = 0; done < n && cb;) {
        const size_t chunk = std::min<size_t>(n - done, cap_frames);
        const size_t samples = chunk * stride;
        const float* out = buf;
        if (silent) {
          std::memset(buf, 0, samples * sizeof(float));
        } else {
          switch (l.kind) {
            case SampleKind::F32:
              out = reinterpret_cast<const float*>(data) + done * stride;
              break;
            case SampleKind::I16:
              int16_to_float(reinterpret_cast<const int16_t*>(data) + done * stride, buf, samples);
              break;
            case SampleKind::I24:
              int24_packed_to_float(data + done * stride * 3, buf, samples);
              break;
            case SampleKind::I32:
              int32_to_float(reinterpret_cast<const int32_t*>(data) + done * stride, buf, samples);
              break;
          }
        }
        cb->on_frames(out, chunk, disc);
        disc = false;
        done += chunk;
      }
      cap->ReleaseBuffer(n);
      if (!run_.load(std::memory_order_acquire)) break;
    }
    if (capture_timed_out(last_packet, GetTickCount64())) failed = true;  // the worker reopens it
  }
  if (failed && run_.load()) {
    fail_gen_.store(gen + 1);
    wk_cv_.notify_one();
  }
  if (mm) AvRevertMmThreadCharacteristics(mm);
}

// ---- WasapiSource ----

WasapiSource::WasapiSource() : p_(std::make_unique<WasapiImpl>()) {}
WasapiSource::~WasapiSource() = default;

bool WasapiSource::open(const CaptureConfig& cfg, std::wstring* err) { return p_->open(cfg, err); }
StreamFormat WasapiSource::format() const { return p_->format(); }
bool WasapiSource::start(CaptureCallbacks* cb) { return p_->start(cb); }
void WasapiSource::stop() { p_->stop(); }
bool WasapiSource::raw_active() const { return p_->raw_active(); }
bool WasapiSource::exclusive() const { return p_->exclusive(); }
std::wstring WasapiSource::device_id() const { return p_->device_id(); }

}  // namespace sl
