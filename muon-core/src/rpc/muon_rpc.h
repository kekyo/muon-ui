/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_type_metadata.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

/**
 * Platform-independent identity for one JavaScript execution context.
 */
struct MuonRpcOwner {
  /** Host-assigned browser or surface identifier. */
  int browser_id = 0;

  /** Host-assigned frame identifier. */
  std::string frame_id;

  /** Renderer-assigned JavaScript context identifier. */
  int context_id = 0;
};

/**
 * Returns true when an RPC owner contains a complete positive identity.
 *
 * @param owner Owner identity to validate.
 */
bool IsValidMuonRpcOwner(const MuonRpcOwner& owner);

/**
 * Returns true when two RPC owners identify the same execution context.
 *
 * @param first First owner identity.
 * @param second Second owner identity.
 */
bool AreEqualMuonRpcOwners(const MuonRpcOwner& first,
                           const MuonRpcOwner& second);

/**
 * Creates the stable state-map key for one RPC owner.
 *
 * @param owner Owner identity to encode.
 */
std::string CreateMuonRpcOwnerKey(const MuonRpcOwner& owner);

/**
 * Platform-owned contiguous memory exposed through an RPC binary value.
 */
class MuonRpcBufferStorage {
 public:
  /** Releases the platform-owned storage. */
  virtual ~MuonRpcBufferStorage() = default;

  /** Returns writable storage memory, or null for an empty allocation. */
  virtual void* GetData() = 0;

  /** Returns read-only storage memory, or null for an empty allocation. */
  virtual const void* GetData() const = 0;

  /** Returns the allocation size in bytes. */
  virtual size_t GetSize() const = 0;
};

/**
 * Creates a process-local owned RPC buffer.
 *
 * @param size Allocation size in bytes.
 * @return Shared ownership of the allocation.
 */
std::shared_ptr<MuonRpcBufferStorage> CreateMuonRpcOwnedBuffer(size_t size);

/**
 * One byte range retained by an RPC value.
 */
struct MuonRpcBinary {
  /** Storage that retains the referenced bytes. */
  std::shared_ptr<MuonRpcBufferStorage> storage;

  /** First referenced byte inside storage. */
  size_t offset = 0;

  /** Number of referenced bytes. */
  size_t size = 0;
};

/**
 * Returns true when a binary value is wholly contained in its storage.
 *
 * @param binary Binary value to validate.
 */
bool IsValidMuonRpcBinary(const MuonRpcBinary& binary);

/**
 * Returns the first writable byte of a valid non-empty binary value.
 *
 * @param binary Binary value to access.
 * @return Referenced memory, or null for invalid and empty values.
 */
void* GetMuonRpcBinaryData(const MuonRpcBinary& binary);

/**
 * Identifies which side owns an RPC function reference.
 */
enum class MuonRpcFunctionKind {
  /** JavaScript function whose source is retained by the renderer. */
  RendererSource,

  /** Native plugin function exposed to the renderer through a proxy. */
  PluginProxy,
};

/**
 * Platform-independent function reference transferred through RPC.
 */
struct MuonRpcFunctionReference {
  /** Side that owns the callable source. */
  MuonRpcFunctionKind kind = MuonRpcFunctionKind::RendererSource;

  /** Renderer context that owns a renderer source. */
  int renderer_context_id = 0;

  /** Renderer-local function identifier. */
  int function_id = 0;

  /** Runtime-wide native function proxy identifier. */
  uint32_t proxy_id = 0;

  /** Unique lease token for the transferred reference. */
  std::string lease_token;

  /** Recursive callable signature. */
  MuonTypeMetadata type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
};

/**
 * Platform-independent value transported by the plugin RPC boundary.
 */
struct MuonRpcValue {
  /** Declared value type, including recursive function signatures. */
  MuonTypeMetadata type = CreateMuonPrimitiveType(MUON_TYPE_VOID);

  /** Whether a nullable string or function value is null. */
  bool is_null = false;

  /** Boolean payload. */
  bool bool_value = false;

  /** Signed 8-bit payload. */
  int8_t i8_value = 0;

  /** Unsigned 8-bit payload. */
  uint8_t u8_value = 0;

  /** Signed 16-bit payload. */
  int16_t i16_value = 0;

  /** Unsigned 16-bit payload. */
  uint16_t u16_value = 0;

  /** Signed 32-bit payload. */
  int32_t i32_value = 0;

  /** Unsigned 32-bit payload. */
  uint32_t u32_value = 0;

  /** Signed 64-bit payload. */
  int64_t i64_value = 0;

  /** Unsigned 64-bit payload. */
  uint64_t u64_value = 0;

  /** 32-bit floating-point payload. */
  float f32_value = 0.0f;

  /** 64-bit floating-point payload. */
  double f64_value = 0.0;

  /** Pointer payload represented without a platform object dependency. */
  uintptr_t pointer_value = 0;

  /** UTF-8 string payload. */
  std::string string_value;

  /** Callable reference payload. */
  MuonRpcFunctionReference function;

  /** Binary view payload. */
  MuonRpcBinary binary;
};

/**
 * Distinguishes direct plugin calls from plugin-owned proxy calls.
 */
enum class MuonRpcCallKind {
  /** Call to a registered plugin function identifier. */
  Plugin,

  /** Call to a plugin-owned function proxy identifier. */
  PluginProxy,
};

/**
 * Optional capability proof attached to a direct plugin call.
 */
struct MuonRpcCapability {
  /** Capability identifier supplied by the generated JavaScript module. */
  std::string id;

  /** Public function path claimed by the capability. */
  std::string function_path;
};

/**
 * Renderer-to-host plugin or proxy invocation.
 */
struct MuonRpcCallRequest {
  /** JavaScript context that initiated the call. */
  MuonRpcOwner owner;

  /** Owner-local positive call identifier. */
  uint32_t call_id = 0;

  /** Invocation target category. */
  MuonRpcCallKind kind = MuonRpcCallKind::Plugin;

  /** Plugin function or native proxy identifier. */
  uint32_t function_id = 0;

  /** Native proxy wrapper lease token for proxy calls. */
  std::string proxy_lease_token;

  /** Capability proof for validated direct calls. */
  MuonRpcCapability capability;

  /** Fully decoded call arguments. */
  std::vector<MuonRpcValue> arguments;
};

/**
 * Host-to-renderer result for a plugin or proxy invocation.
 */
struct MuonRpcCallResult {
  /** JavaScript context that initiated the call. */
  MuonRpcOwner owner;

  /** Owner-local call identifier being completed. */
  uint32_t call_id = 0;

  /** Whether the invocation completed successfully. */
  bool success = false;

  /** Failure diagnostic when success is false. */
  std::string error_message;

  /** Successful result value. */
  MuonRpcValue value;
};

/**
 * Renderer-to-host request to cancel one pending invocation.
 */
struct MuonRpcCallCancel {
  /** JavaScript context that initiated the call. */
  MuonRpcOwner owner;

  /** Owner-local positive call identifier being cancelled. */
  uint32_t call_id = 0;
};

/**
 * Host-to-renderer invocation of a renderer-owned function source.
 */
struct MuonRpcRendererFunctionCall {
  /** JavaScript context that owns the source function. */
  MuonRpcOwner owner;

  /** Runtime-wide callback call identifier. */
  uint32_t call_id = 0;

  /** Renderer-local source function identifier. */
  int function_id = 0;

  /** Whether the native caller supplied a result completion. */
  bool expects_result = false;

  /** Recursive signature of the renderer-owned function. */
  MuonTypeMetadata function_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);

  /** Fully decoded callback arguments. */
  std::vector<MuonRpcValue> arguments;
};

/**
 * Renderer-to-host result for a renderer-owned function invocation.
 */
struct MuonRpcRendererFunctionResult {
  /** JavaScript context that owns the source function. */
  MuonRpcOwner owner;

  /** Runtime-wide callback call identifier. */
  uint32_t call_id = 0;

  /** Whether the renderer invocation completed successfully. */
  bool success = false;

  /** Failure diagnostic when success is false. */
  std::string error_message;

  /** Successful renderer result value. */
  MuonRpcValue value;
};

/**
 * Renderer source lease transition requested by the host runtime.
 */
struct MuonRpcRendererFunctionLease {
  /** JavaScript context that owns the source function. */
  MuonRpcOwner owner;

  /** Renderer-local source function identifier. */
  int function_id = 0;

  /** Unique lease token for this source reference. */
  std::string lease_token;

  /** Whether the lease is being acquired instead of released. */
  bool acquire = false;
};

/**
 * Acknowledges that a renderer function result transfer was consumed.
 */
struct MuonRpcRendererFunctionResultConsumed {
  /** JavaScript context that produced the result. */
  MuonRpcOwner owner;

  /** Runtime-wide callback call identifier. */
  uint32_t call_id = 0;
};

/**
 * Releases one renderer wrapper lease for a native plugin function proxy.
 */
struct MuonRpcPluginProxyRelease {
  /** JavaScript context that owns the wrapper. */
  MuonRpcOwner owner;

  /** Runtime-wide native function proxy identifier. */
  uint32_t proxy_id = 0;

  /** Unique renderer wrapper lease token. */
  std::string lease_token;
};

/**
 * Releases all RPC resources retained for a JavaScript context.
 */
struct MuonRpcContextReleased {
  /** JavaScript context being released. */
  MuonRpcOwner owner;
};

/**
 * Complete platform-independent message family used by plugin RPC.
 */
using MuonRpcMessage = std::variant<
    MuonRpcCallRequest,
    MuonRpcCallResult,
    MuonRpcCallCancel,
    MuonRpcRendererFunctionCall,
    MuonRpcRendererFunctionResult,
    MuonRpcRendererFunctionLease,
    MuonRpcRendererFunctionResultConsumed,
    MuonRpcPluginProxyRelease,
    MuonRpcContextReleased>;

/**
 * Metadata retained while one client-side call is pending.
 */
struct MuonRpcPendingCall {
  /** JavaScript context that initiated the call. */
  MuonRpcOwner owner;

  /** Owner-local call identifier. */
  uint32_t call_id = 0;

  /** Result type expected by the caller. */
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
};

/**
 * Outcome of matching a result to client-side pending state.
 */
enum class MuonRpcCallCompletionStatus {
  /** The pending call was found, returned, and retired. */
  Completed,

  /** No pending or previously completed call has this identifier. */
  UnknownCall,

  /** The call identifier was already completed. */
  Duplicate,

  /** The call exists but belongs to another JavaScript context. */
  OwnerMismatch,
};

/**
 * CEF-independent client-side call identifier and pending-call state.
 */
class MuonRpcClientState final {
 public:
  /**
   * Creates client state with a positive monotonically increasing id range.
   *
   * @param maximum_call_id Largest call id that may be allocated.
   */
  explicit MuonRpcClientState(
      uint32_t maximum_call_id =
          static_cast<uint32_t>(std::numeric_limits<int>::max()));

  /**
   * Allocates and retains one pending call.
   *
   * @param owner JavaScript context initiating the call.
   * @param return_type Expected result type.
   * @param call_id Receives the allocated positive identifier.
   * @param error_message Receives a validation or exhaustion diagnostic.
   * @return true when pending state was created.
   */
  bool BeginCall(const MuonRpcOwner& owner,
                 const MuonTypeMetadata& return_type,
                 uint32_t* call_id,
                 std::string* error_message);

  /**
   * Matches and retires one completed call.
   *
   * @param owner JavaScript context named by the result.
   * @param call_id Result call identifier.
   * @param pending_call Receives retained call metadata when completed.
   * @return Matching outcome.
   */
  MuonRpcCallCompletionStatus CompleteCall(
      const MuonRpcOwner& owner,
      uint32_t call_id,
      MuonRpcPendingCall* pending_call);

  /**
   * Removes a pending call that could not be sent.
   *
   * @param owner JavaScript context that initiated the call.
   * @param call_id Pending call identifier.
   * @param pending_call Receives removed metadata when non-null.
   * @return true when matching pending state was removed.
   */
  bool CancelCall(const MuonRpcOwner& owner,
                  uint32_t call_id,
                  MuonRpcPendingCall* pending_call);

  /**
   * Removes and returns every pending call owned by one context.
   *
   * @param owner JavaScript context being released.
   */
  std::vector<MuonRpcPendingCall> ReleaseOwner(const MuonRpcOwner& owner);

  /** Returns the number of calls awaiting a result. */
  size_t GetPendingCallCount() const;

 private:
  uint32_t maximum_call_id_ = 0;
  uint32_t next_call_id_ = 1;
  std::map<uint32_t, MuonRpcPendingCall> pending_calls_;
  std::set<uint32_t> completed_call_ids_;
};
