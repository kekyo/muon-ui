#include "muon_service_api.h"
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include <cardio.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <charconv>
#include <cstring>
#include <optional>
#include <string>

#if defined(_WIN32)
using FixtureSocket = SOCKET;
static constexpr auto invalid_socket = INVALID_SOCKET;
static void CloseSocket(FixtureSocket socket) { closesocket(socket); }
static bool RetrySocket() {
  const auto error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINTR;
}
static unsigned long ProcessId() { return GetCurrentProcessId(); }

static cardio::promise<void> WaitSocket(FixtureSocket socket, bool writing,
    bool listener, cardio::cancellation cancellation) {
  struct Event {
    FixtureSocket socket;
    WSAEVENT handle;
    ~Event() { WSAEventSelect(socket, nullptr, 0); WSACloseEvent(handle); }
  } event{socket, WSACreateEvent()};
  if (event.handle == WSA_INVALID_EVENT || WSAEventSelect(socket, event.handle,
      (listener ? FD_ACCEPT : writing ? FD_WRITE : FD_READ) | FD_CLOSE) != 0)
    throw std::runtime_error("Fixture socket readiness registration failed");
  // No native OVERLAPPED operation is pending here: cancellation only releases
  // an event wait, then the scope detaches notification before closing it.
  co_await cardio::from_win32_handle(event.handle, cancellation);
  WSANETWORKEVENTS events{};
  if (WSAEnumNetworkEvents(socket, event.handle, &events) != 0)
    throw std::runtime_error("Fixture socket readiness collection failed");
}
#else
using FixtureSocket = int;
static constexpr auto invalid_socket = -1;
static void CloseSocket(FixtureSocket socket) { close(socket); }
static bool RetrySocket() { return errno == EAGAIN || errno == EINTR; }
static pid_t ProcessId() { return getpid(); }
static cardio::promise<void> WaitSocket(FixtureSocket socket, bool writing,
    bool, cardio::cancellation cancellation) {
  co_await cardio::from_fd(socket,
      writing ? cardio::fd_event::write : cardio::fd_event::read, cancellation);
}
#endif

struct Fixture {
  const muon_service_context* context;
  FixtureSocket listener = invalid_socket;
  FixtureSocket client = invalid_socket;
  uint64_t count = 0;
  cardio::cancellation_source cancellation{};
  std::optional<cardio::promise<void>> work{};
  std::optional<cardio::promise<void>> stopping{};
  muon_service_stop_completion stop_completion = nullptr;
  void* stop_data = nullptr;
};

static std::string LabelHex(const muon_service_context* context) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (int32_t index = 0; index < context->config_count; ++index) {
    const auto& entry = context->config_entries[index];
    if (std::strcmp(entry.key, "label") != 0) continue;
    for (const unsigned char value : std::string(entry.value)) {
      result += digits[value >> 4];
      result += digits[value & 15];
    }
  }
  return result;
}

static cardio::promise<void> Serve(Fixture* state) {
  const auto cancellation = state->cancellation.get_cancellation();
  try {
    for (;;) {
      co_await WaitSocket(state->listener, false, true, cancellation);
#if defined(_WIN32)
      state->client = accept(state->listener, nullptr, nullptr);
#else
      state->client = accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
      if (state->client == invalid_socket) continue;
      std::string input;
      for (;;) {
        co_await WaitSocket(state->client, false, false, cancellation);
        char buffer[256];
        const auto size = recv(state->client, buffer, sizeof(buffer), 0);
        if (size == 0) break;
        if (size < 0) { if (RetrySocket()) continue; break; }
        input.append(buffer, static_cast<size_t>(size));
        const auto end = input.find('\n');
        if (end == std::string::npos) {
          if (input.size() > 128) break;
          continue;
        }
        const auto nonce = input.substr(0, end);
        input.erase(0, end + 1);
        if (nonce.find_first_not_of("0123456789abcdef-") != std::string::npos) break;
        const auto reply = "{\"nonce\":\"" + nonce + "\",\"pid\":" + std::to_string(ProcessId()) +
            ",\"generation\":\"" + std::to_string(state->context->generation) +
            "\",\"labelHex\":\"" + LabelHex(state->context) +
            "\",\"count\":" + std::to_string(++state->count) + "}\n";
        size_t sent = 0;
        while (sent < reply.size()) {
          co_await WaitSocket(state->client, true, false, cancellation);
#if defined(_WIN32)
          const auto written = send(state->client, reply.data() + sent,
                                    static_cast<int>(reply.size() - sent), 0);
#else
          const auto written = send(state->client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
#endif
          if (written < 0 && RetrySocket()) continue;
          if (written <= 0) throw std::runtime_error("Fixture response failed");
          sent += static_cast<size_t>(written);
        }
      }
      CloseSocket(state->client);
      state->client = invalid_socket;
    }
  } catch (const cardio::canceled_exception&) {
    // Stop cancellation is the barrier after which no socket work remains.
  } catch (const std::exception& error) {
    state->context->host->log(state->context->host->user_data, MUON_LOG_LEVEL_ERROR, error.what());
  }
}

static void FinishStop(void* data) {
  auto* state = static_cast<Fixture*>(data);
  const auto completion = state->stop_completion;
  auto* completion_data = state->stop_data;
  // Both coroutine frames must be destroyed before the host may unload this
  // library. The deferred task runs after StopAsync has returned completely.
  delete state;
  completion(completion_data);
}

static cardio::promise<void> StopAsync(Fixture* state) {
  state->cancellation.cancel();
  co_await *state->work;
  if (state->client != invalid_socket) CloseSocket(state->client);
  CloseSocket(state->listener);
#if defined(_WIN32)
  WSACleanup();
#endif
  state->context->host->post(state->context->host->user_data, FinishStop, state);
}

static void Stop(void* data, muon_service_stop_completion completion, void* completion_data) {
  auto* state = static_cast<Fixture*>(data);
  state->stop_completion = completion;
  state->stop_data = completion_data;
  state->stopping.emplace(StopAsync(state));
}

static void Start(const muon_service_context* context,
    muon_service_start_completion completion, void* data) {
  unsigned int port = 0;
  for (int32_t index = 0; index < context->config_count; ++index) {
    const auto& entry = context->config_entries[index];
    if (std::strcmp(entry.key, "port") != 0) continue;
    const auto end = entry.value + std::strlen(entry.value);
    const auto parsed = std::from_chars(entry.value, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end) port = 0;
  }
  if (port == 0 || port > 65535) {
    completion(data, nullptr, "Fixture requires a TCP port from 1 to 65535");
    return;
  }
  auto* state = new Fixture{context};
#if defined(_WIN32)
  WSADATA winsock{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    delete state;
    completion(data, nullptr, "Fixture Winsock initialization failed");
    return;
  }
  state->listener = socket(AF_INET, SOCK_STREAM, 0);
  const int exclusive = 1;
  setsockopt(state->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
  state->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  const int reuse = 1;
  setsockopt(state->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (state->listener == invalid_socket || bind(state->listener, reinterpret_cast<sockaddr*>(&address),
      sizeof(address)) != 0 ||
      listen(state->listener, 4) != 0) {
#if defined(_WIN32)
    const auto error = "Fixture socket failed: " + std::to_string(WSAGetLastError());
    if (state->listener != invalid_socket) CloseSocket(state->listener);
    WSACleanup();
#else
    const auto error = std::string(std::strerror(errno));
    if (state->listener != invalid_socket) CloseSocket(state->listener);
#endif
    delete state;
    completion(data, nullptr, error.c_str());
    return;
  }
  state->work.emplace(Serve(state));
  const muon_service_instance instance{state, Stop};
  completion(data, &instance, nullptr);
}

static const muon_service_factory factory{MUON_SERVICE_ABI_VERSION, Start};

/** @return Factory for independent socket workers shared by Android and PC tests. */
extern "C" MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory() {
  return &factory;
}
