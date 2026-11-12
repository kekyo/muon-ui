#include "muon_service_api.h"
#include <cardio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <optional>
#include <string>

struct Fixture {
  const muon_service_context* context;
  int listener = -1;
  int client = -1;
  uint64_t count = 0;
  cardio::cancellation_source cancellation{};
  std::optional<cardio::promise<void>> work{};
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
      co_await cardio::from_fd(state->listener, cardio::fd_event::read, cancellation);
      state->client = accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (state->client < 0) continue;
      std::string input;
      for (;;) {
        co_await cardio::from_fd(state->client, cardio::fd_event::read, cancellation);
        char buffer[256];
        const auto size = recv(state->client, buffer, sizeof(buffer), 0);
        if (size == 0) break;
        if (size < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
        input.append(buffer, static_cast<size_t>(size));
        const auto end = input.find('\n');
        if (end == std::string::npos) {
          if (input.size() > 128) break;
          continue;
        }
        const auto nonce = input.substr(0, end);
        input.erase(0, end + 1);
        if (nonce.find_first_not_of("0123456789abcdef-") != std::string::npos) break;
        const auto reply = "{\"nonce\":\"" + nonce + "\",\"pid\":" + std::to_string(getpid()) +
            ",\"generation\":\"" + std::to_string(state->context->generation) +
            "\",\"labelHex\":\"" + LabelHex(state->context) +
            "\",\"count\":" + std::to_string(++state->count) + "}\n";
        size_t sent = 0;
        while (sent < reply.size()) {
          co_await cardio::from_fd(state->client, cardio::fd_event::write, cancellation);
          const auto written = send(state->client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
          if (written < 0 && (errno == EAGAIN || errno == EINTR)) continue;
          if (written <= 0) throw std::runtime_error("Fixture response failed");
          sent += static_cast<size_t>(written);
        }
      }
      close(state->client);
      state->client = -1;
    }
  } catch (const cardio::canceled_exception&) {
    // Stop cancellation is the barrier after which no socket work remains.
  } catch (const std::exception& error) {
    state->context->host->log(state->context->host->user_data, MUON_LOG_LEVEL_ERROR, error.what());
  }
}

static cardio::promise<void> StopAsync(Fixture* state,
    muon_service_stop_completion completion, void* data) {
  state->cancellation.cancel();
  co_await *state->work;
  if (state->client >= 0) close(state->client);
  close(state->listener);
  delete state;
  completion(data);
}

static void Stop(void* state, muon_service_stop_completion completion, void* data) {
  cardio::fire_and_forget(StopAsync(static_cast<Fixture*>(state), completion, data));
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
  state->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  const int reuse = 1;
  setsockopt(state->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (state->listener < 0 || bind(state->listener, reinterpret_cast<sockaddr*>(&address),
      sizeof(address)) != 0 ||
      listen(state->listener, 4) != 0) {
    const auto error = std::string(std::strerror(errno));
    if (state->listener >= 0) close(state->listener);
    delete state;
    completion(data, nullptr, error.c_str());
    return;
  }
  state->work.emplace(Serve(state));
  const muon_service_instance instance{state, Stop};
  completion(data, &instance, nullptr);
}

static const muon_service_factory factory{MUON_SERVICE_ABI_VERSION, Start};

/** @return Factory for independent socket workers used only by the test APK. */
extern "C" MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory() {
  return &factory;
}
