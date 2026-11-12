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
#else
using FixtureSocket = int;
static constexpr auto invalid_socket = -1;
static void CloseSocket(FixtureSocket socket) { close(socket); }
static bool RetrySocket() { return errno == EAGAIN || errno == EINTR; }
static pid_t ProcessId() { return getpid(); }
#endif

struct Fixture {
  const muon_service_context* context;
  FixtureSocket listener = invalid_socket;
  FixtureSocket client = invalid_socket;
#if defined(_WIN32)
  WSAEVENT waiting = WSA_INVALID_EVENT;
#endif
  uint64_t count = 0;
  cardio::cancellation_source cancellation{};
  std::optional<cardio::promise<void>> work{};
  std::optional<cardio::promise<void>> stopping{};
  muon_service_stop_completion stop_completion = nullptr;
  void* stop_data = nullptr;
};

#if defined(_WIN32)
static cardio::promise<void> WaitSocket(Fixture& state, FixtureSocket socket,
    bool writing, bool listener) {
  struct Event {
    Fixture& state;
    FixtureSocket socket;
    WSAEVENT handle;
    ~Event() {
      state.waiting = WSA_INVALID_EVENT;
      WSAEventSelect(socket, nullptr, 0);
      WSACloseEvent(handle);
    }
  } event{state, socket, WSACreateEvent()};
  state.waiting = event.handle;
  if (event.handle == WSA_INVALID_EVENT || WSAEventSelect(socket, event.handle,
      (listener ? FD_ACCEPT : writing ? FD_WRITE : FD_READ) | FD_CLOSE) != 0)
    throw std::runtime_error("Fixture socket readiness registration failed");
  // Stop signals this event on the owner dispatcher. Await the actual native
  // wait completion before closing the HANDLE: cancelling only the promise
  // could leave the dispatcher's wait thread still observing that handle.
  co_await cardio::from_win32_handle(event.handle);
  state.cancellation.get_cancellation().throw_if_cancellation_requested();
  WSANETWORKEVENTS events{};
  if (WSAEnumNetworkEvents(socket, event.handle, &events) != 0)
    throw std::runtime_error("Fixture socket readiness collection failed");
}
#else
static cardio::promise<void> WaitSocket(Fixture& state, FixtureSocket socket,
    bool writing, bool) {
  co_await cardio::from_fd(socket,
      writing ? cardio::fd_event::write : cardio::fd_event::read,
      state.cancellation.get_cancellation());
}
#endif

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
      if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
#if defined(_WIN32)
      state->client = accept(state->listener, nullptr, nullptr);
#else
      state->client = accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
      if (state->client == invalid_socket) {
        if (!RetrySocket()) throw std::runtime_error("Fixture accept failed");
        co_await WaitSocket(*state, state->listener, false, true);
        continue;
      }
      std::string input;
      for (;;) {
        if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
        char buffer[256];
        const auto size = recv(state->client, buffer, sizeof(buffer), 0);
        if (size == 0) break;
        if (size < 0) {
          if (!RetrySocket()) break;
          co_await WaitSocket(*state, state->client, false, false);
          continue;
        }
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
          if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
          // Winsock records FD_WRITE initially and after WSAEWOULDBLOCK, not
          // before every send. Attempt nonblocking I/O before awaiting readiness;
          // likewise recv observes EOF before a previously delivered FD_CLOSE
          // could be discarded by a new event registration.
#if defined(_WIN32)
          const auto written = send(state->client, reply.data() + sent,
                                    static_cast<int>(reply.size() - sent), 0);
#else
          const auto written = send(state->client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
#endif
          if (written < 0 && RetrySocket()) {
            co_await WaitSocket(*state, state->client, true, false);
            continue;
          }
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
#if defined(_WIN32)
  if (state->waiting != WSA_INVALID_EVENT) WSASetEvent(state->waiting);
#endif
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

struct CallbackReply {
  muon_service_result_completion complete;
  void* data;
};

static void CompleteCallback(void* data, const muon_service_value* value,
    const muon_service_error* error) {
  const auto reply = *static_cast<CallbackReply*>(data);
  delete static_cast<CallbackReply*>(data);
  reply.complete(reply.data, value, error);
}

static void Invoke(void* data, const muon_service_call_context* call,
    const char* method, const muon_service_value* arguments, size_t,
    muon_service_result_completion complete, void* completion_data) {
  auto* state = static_cast<Fixture*>(data);
  if (std::strcmp(method, "callback") == 0) {
    auto* reply = new CallbackReply{complete, completion_data};
    if (!call->callback(call->user_data, arguments[1].data.callback_id,
        arguments, 1, CompleteCallback, reply)) {
      delete reply;
      const muon_service_error error{"resource-exhausted", "Callback was not accepted"};
      complete(completion_data, nullptr, &error);
    }
  } else if (std::strcmp(method, "emit") == 0) {
    const auto* host = state->context->host;
    if (host->emit(host->user_data, "progress", arguments)) {
      const muon_service_value result{MUON_SERVICE_VALUE_VOID, {}};
      complete(completion_data, &result, nullptr);
    } else {
      const muon_service_error error{"resource-exhausted", "Event was not accepted"};
      complete(completion_data, nullptr, &error);
    }
  } else {
    complete(completion_data, arguments, nullptr);
  }
}

static void Cancel(void*, uint64_t) {
  // The only asynchronous method awaits a page callback. Host cancellation
  // settles that callback, and CompleteCallback then completes the method.
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
  unsigned long nonblocking = 1;
  const bool nonblocking_ready = state->listener != invalid_socket &&
      ioctlsocket(state->listener, FIONBIO, &nonblocking) == 0;
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
  if (state->listener == invalid_socket ||
#if defined(_WIN32)
      !nonblocking_ready ||
#endif
      bind(state->listener, reinterpret_cast<sockaddr*>(&address),
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
  const muon_service_instance instance{state, Stop, Invoke, Cancel};
  completion(data, &instance, nullptr);
}

static const char interface_json[] = R"({"schemaVersion":1,"methods":{
  "echo":{"parameters":["string"],"returns":"string","errors":[]},
  "echoBytes":{"parameters":["bytes"],"returns":"bytes","errors":[]},
  "echoInt64":{"parameters":["int64"],"returns":"int64","errors":[]},
  "echoUint64":{"parameters":["uint64"],"returns":"uint64","errors":[]},
  "callback":{"parameters":["string",{"callback":{"parameters":["string"],"returns":"string"}}],"returns":"string","errors":[]},
  "emit":{"parameters":["int32"],"returns":"void","errors":[]}
},"events":{"progress":"int32"}})";

static const muon_service_factory factory{MUON_SERVICE_ABI_VERSION, Start, interface_json};

/** @return Factory for independent socket workers shared by Android and PC tests. */
extern "C" MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory() {
  return &factory;
}
