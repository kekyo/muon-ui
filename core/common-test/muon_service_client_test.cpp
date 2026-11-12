/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_client.h"
#include <cardio.h>
#include <cassert>
#include <iostream>

struct Channel {
  std::vector<std::string> sent;
  std::optional<cardio::promise_source<std::string>> next;
  std::optional<cardio::promise_source<void>> written;
  bool closed = false;
  bool fail_write = false;
  int connects = 0;

  MuonServiceTransport Transport() {
    return {
        [this]() -> cardio::promise<std::string> {
          next.emplace();
          co_return co_await next->get_promise();
        },
        [this](std::string frame) -> cardio::promise<void> {
          if (fail_write) throw std::runtime_error("write failed");
          sent.push_back(std::move(frame));
          if (written) {
            auto current = std::move(*written);
            written.reset();
            current.resolve();
          }
          co_return;
        },
        [this] {
          closed = true;
          if (next) {
            auto current = std::move(*next);
            next.reset();
            current.try_reject(std::make_exception_ptr(std::runtime_error("channel closed")));
          }
        }};
  }

  cardio::promise<void> WaitSent(size_t count) {
    while (sent.size() < count) {
      written.emplace();
      co_await written->get_promise();
    }
  }

  void Emit(const std::string& data) {
    auto current = std::move(*next);
    next.reset();
    current.resolve(data);
  }
};

static cardio::promise<std::string> Failure(cardio::promise<std::string> pending) {
  try { co_await pending; }
  catch (const std::exception& error) { co_return error.what(); }
  co_return "";
}

static cardio::promise<void> Tests(bool& passed) {
  {
    Channel channel;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      co_return channel.Transport();
    });
    auto enabled = client.Call("setAutostart", R"({"serviceId":"worker","enabled":false,"op":"shutdown"})");
    co_await channel.WaitSent(1);
    assert(channel.sent.size() == 1);
    assert(channel.sent[0].find("\"enabled\":false") != std::string::npos);
    assert(channel.sent[0].find("shutdown") == std::string::npos);
    channel.Emit("{\"requestId\":\"1\",\"result\":null}\n");
    assert(co_await enabled == "null");
    auto next = client.Call("watchState", R"({"serviceId":"worker","operation":"next","token":"9007199254740993","owner":"forged"})");
    co_await channel.WaitSent(2);
    assert(channel.sent[1].find("9007199254740993") != std::string::npos);
    assert(channel.sent[1].find("forged") == std::string::npos);
    channel.Emit("{\"requestId\":\"2\",\"result\":{\"state\":null}}\n");
    assert(co_await next == "{\"state\":null}");
    bool rejected = false;
    try { co_await client.Call("setAutostart", R"({"serviceId":"worker","enabled":"false"})"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    assert(channel.sent.size() == 2);
    co_await client.Close();
  }
  {
    Channel channel;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      ++channel.connects;
      co_return channel.Transport();
    });
    auto first = client.Call("acquire", "{\"serviceId\":\"worker\"}");
    auto second = client.Call("getState", "{\"serviceId\":\"worker\"}");
    co_await channel.WaitSent(2);
    assert(channel.connects == 1 && channel.sent.size() == 2);
    channel.Emit("{\"requestId\":\"2\",\"result\":{\"state\":\"running\"}}\n");
    assert(co_await second == "{\"state\":\"running\"}");
    channel.Emit("{\"requestId\":\"1\",\"result\":{\"serviceId\":\"worker\","
                 "\"refId\":\"9007199254740993\",\"generation\":\"1\"}}\n");
    assert((co_await first).find("9007199254740993") != std::string::npos);
    auto close1 = client.Close();
    auto close2 = client.Close();
    co_await close1;
    co_await close2;
    co_await client.Close();
    assert(channel.closed);
  }
  {
    Channel channel;
    cardio::promise_source<MuonServiceTransport> connecting;
    MuonServiceClient client([&](cardio::cancellation) {
      return connecting.get_promise();
    });
    auto acquisition = client.Call("acquire", "{\"serviceId\":\"worker\"}");
    auto closing = client.Close();
    connecting.resolve(channel.Transport());
    bool failed = false;
    try { co_await acquisition; } catch (const std::exception&) { failed = true; }
    assert(failed);
    co_await closing;
    assert(channel.closed && channel.sent.empty());
  }
  {
    Channel channel;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      ++channel.connects;
      co_return channel.Transport();
    });
    auto pending = Failure(client.Call("acquire", "{\"serviceId\":\"worker\"}"));
    auto observation = Failure(client.Call("getState", "{\"serviceId\":\"worker\"}"));
    co_await channel.WaitSent(2);
    channel.Emit("");
    assert(co_await pending == "Service host disconnected");
    assert(co_await observation == "Service host disconnected");
    bool failed = false;
    try { co_await client.Call("acquire", "{\"serviceId\":\"worker\"}"); }
    catch (const std::exception&) { failed = true; }
    assert(failed && channel.connects == 1);
    co_await client.Close();
  }
  {
    Channel channel;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      co_return channel.Transport();
    });
    auto pending = client.Call("acquire", "{\"serviceId\":\"worker\"}");
    co_await channel.WaitSent(1);
    channel.Emit("{\"requestId\":\"1\"}\n");
    bool rejected = false;
    try { co_await pending; }
    catch (const std::exception& error) {
      rejected = std::string(error.what()) == "Missing service protocol value";
    }
    assert(rejected && channel.closed);
    co_await client.Close();
  }
  {
    Channel channel;
    channel.fail_write = true;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      co_return channel.Transport();
    });
    bool failed = false;
    try { co_await client.Call("acquire", "{\"serviceId\":\"worker\"}"); }
    catch (const std::exception&) { failed = true; }
    assert(failed && channel.closed);
    co_await client.Close();
  }
  {
    Channel channel;
    MuonServiceClient client([&](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      co_return channel.Transport();
    });
    bool rejected = false;
    try { co_await client.Call("shutdown", "{}"); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected && channel.sent.empty());
    auto request = client.Call("getState",
        "{\"serviceId\":\"worker\",\"op\":\"shutdown\",\"owner\":\"forged\",\"requestId\":\"forged\"}");
    co_await channel.WaitSent(1);
    assert(channel.sent.size() == 1);
    assert(channel.sent[0].find("shutdown") == std::string::npos);
    assert(channel.sent[0].find("forged") == std::string::npos);
    channel.Emit("{\"requestId\":\"1\",\"error\":{\"code\":\"unavailable\",\"message\":\"failed\"}}\n");
    rejected = false;
    try { co_await request; } catch (const std::exception& error) {
      rejected = std::string(error.what()) == "failed";
    }
    assert(rejected);
    co_await client.Close();
  }
  std::cout << "service IPC client: passed\n";
  passed = true;
}

int main() {
  cardio::dispatcher_host dispatcher;
  bool passed = false;
  auto test = Tests(passed);
  dispatcher.park();
  return passed ? 0 : 1;
}
