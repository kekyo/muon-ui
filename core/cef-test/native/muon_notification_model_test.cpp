/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_model.h"
#include <iostream>
#include <stdexcept>

using namespace muon_internal;

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void RunTests() {
  const auto post = ParseMuonNotificationPost(
      R"({"id":"inbox","title":"Message","body":"Plain <text>","data":{"version":1},"actions":[{"id":"default","title":"Reply"}]})",
      "messages");
  Expect(post.category == "messages", "default category was not applied");
  Expect(post.data == R"({"version":1})", "payload changed");
  for (const auto& invalid : {
           R"({"id":" ","title":"title","body":""})",
           R"({"id":"id","title":"title","body":"","unknown":1})",
           R"({"id":"id","title":"title","body":"","actions":[{"id":"x","title":"A"},{"id":"x","title":"B"}]})"}) {
    auto rejected = false;
    try { (void)ParseMuonNotificationPost(invalid, ""); }
    catch (const std::exception&) { rejected = true; }
    Expect(rejected, "invalid post was accepted");
  }
  auto state = MuonNotificationSnapshot{};
  ReserveMuonNotification(state, post, "old-token", 1);
  ReserveMuonNotification(state, post, "new-token", 2);
  Expect(state.revisions.size() == 1, "same ID update consumed another slot");
  Expect(!ActivateMuonNotification(state, "old-token", std::nullopt, 3),
         "late old action activated the new revision");
  Expect(ActivateMuonNotification(state, "new-token", std::string("default"), 3),
         "named action did not create an event");
  Expect(!ActivateMuonNotification(state, "new-token", std::nullopt, 4),
         "duplicate action created another event");
  Expect(state.pending.size() == 1 && state.revisions.empty(),
         "first action did not exchange its reserved slot");
  const auto events = SerializeMuonNotificationEvents(state, 0);
  Expect(events.find(R"("type":"action")") != std::string::npos &&
             events.find(R"("actionId":"default")") != std::string::npos &&
             events.find("cursor") == std::string::npos,
         "public event fields are incorrect");
  state = ParseMuonNotificationSnapshot(SerializeMuonNotificationSnapshot(state));
  Expect(state.pending.size() == 1 && state.pending[0].data == post.data,
         "pending event was not retained across snapshot reload");
  ReserveMuonNotification(state, post, "latest-token", 5);
  Expect(state.pending.size() == 1, "new post discarded old pending event");
  AcknowledgeMuonNotification(state, "new-token", 6);
  state = ParseMuonNotificationSnapshot(SerializeMuonNotificationSnapshot(state));
  Expect(state.pending.empty() && !ActivateMuonNotification(state, "new-token", std::nullopt, 7),
         "ACK was lost across snapshot reload");
  for (int i = 0; i < 255; ++i) {
    auto other = post;
    other.id = std::to_string(i);
    ReserveMuonNotification(state, other, "capacity-" + other.id, 8);
  }
  auto rejected = false;
  try {
    auto extra = post;
    extra.id = "overflow";
    ReserveMuonNotification(state, extra, "overflow-token", 8);
  } catch (const std::exception&) { rejected = true; }
  Expect(rejected && state.revisions.size() == 256, "full capacity was not rejected");
  PruneMuonNotifications(state, 8 + 30LL * 24 * 60 * 60 * 1000);
  Expect(state.revisions.empty(), "30-day revisions did not expire");
  rejected = false;
  try { (void)ParseMuonNotificationSnapshot("{corrupt"); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "corrupt snapshot was silently reset");

  const auto prefix = std::string(R"({"id":"id","title":"title","body":"","data":)");
  (void)ParseMuonNotificationPost(prefix + std::string(64, '[') + "0" + std::string(64, ']') + "}", "");
  (void)ParseMuonNotificationPost(prefix + "\"" + std::string(16382, 'x') + "\"}", "");
  for (const auto& invalid : {
           prefix + std::string(65, '[') + std::string(65, ']') + "}",
           prefix + "\"" + std::string(16383, 'x') + "\"}",
           std::string(R"({"id":"\u3000\ufeff","title":"title","body":""})"),
           std::string(R"({"id":"id","title":"title","body":"\u0000"})")}) {
    rejected = false;
    try { (void)ParseMuonNotificationPost(invalid, ""); }
    catch (const std::exception&) { rejected = true; }
    Expect(rejected, "JSON size/depth or Unicode boundary was accepted");
  }
  state = {};
  for (int index = 0; index < 1025; ++index) {
    const auto token = "ack-" + std::to_string(index);
    ReserveMuonNotification(state, post, token, 1);
    Expect(ActivateMuonNotification(state, token, std::nullopt, 2), "ACK fixture did not activate");
    AcknowledgeMuonNotification(state, token, 3);
  }
  state = ParseMuonNotificationSnapshot(SerializeMuonNotificationSnapshot(state));
  Expect(state.acknowledged.size() == 1024 && state.acknowledged.front().token == "ack-1",
         "ACK retention limit was not preserved across reopen");
  Expect(!ActivateMuonNotification(state, "ack-0", std::nullopt, 4),
         "evicted ACK regenerated an event");
  PruneMuonNotifications(state, 2 + 30LL * 24 * 60 * 60 * 1000);
  Expect(state.acknowledged.size() == 1024, "ACK expired before 30 days");
  PruneMuonNotifications(state, 3 + 30LL * 24 * 60 * 60 * 1000);
  Expect(state.acknowledged.empty(), "ACK did not expire at 30 days");
}

int main() {
  try { RunTests(); }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
