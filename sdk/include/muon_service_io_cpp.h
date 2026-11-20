/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "muon_service_api.h"
#include <cardio.h>
#include <memory>
#include <string>
#include <vector>

namespace muon {
/** Owned result of a service I/O operation, safe to retain across suspension. */
struct service_io_result {
  std::string metadata; /**< JSON metadata. */
  std::vector<uint8_t> body; /**< Binary result. */
  std::string code, message; /**< Empty on success. */
};
/** @param io Table borrowed from this generation. @param request Operation parameters and selected signal.
 * @return A cardio promise completed on the calling dispatcher with copied metadata, bytes and error. */
inline cardio::promise<service_io_result> request_service_io(const muon_service_io& io, const muon_service_io_request& request) {
  using source = cardio::promise_source<service_io_result>;
  auto completion = std::make_unique<source>();
  auto promise = completion->get_promise();
  io.request(io.user_data, &request, [](void* data, const muon_service_io_result* result) {
    std::unique_ptr<source> owner(static_cast<source*>(data));
    service_io_result value;
    if (result->metadata_json) value.metadata = result->metadata_json;
    if (result->body_size) value.body.assign(result->body, result->body + result->body_size);
    if (result->code) value.code = result->code;
    if (result->message) value.message = result->message;
    owner->resolve(std::move(value));
  }, completion.release());
  return promise;
}
}
