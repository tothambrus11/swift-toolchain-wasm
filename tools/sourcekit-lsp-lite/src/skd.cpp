// The only part of the server that touches sourcekitd's C API.

#include "skd.h"

#include "sourcekitd.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace lite::skd {

namespace {

/// sourcekitd_request_array_set_*'s "append" index. The header's macro is a
/// C cast; this is the same value without one.
constexpr std::size_t ArrayAppend = static_cast<std::size_t>(-1);

struct RequestRelease {
  void operator()(void *Object) const noexcept { sourcekitd_request_release(Object); }
};
/// An owned request object (dictionary, array or scalar).
using RequestObject = std::unique_ptr<void, RequestRelease>;

struct ResponseDispose {
  void operator()(void *Response) const noexcept { sourcekitd_response_dispose(Response); }
};
/// An owned response.
using ResponseObject = std::unique_ptr<void, ResponseDispose>;

sourcekitd_uid_t uid(const std::string &Name) {
  // uids are interned for the life of the process; so is this cache.
  static std::unordered_map<std::string, sourcekitd_uid_t> Cache;
  const auto It = Cache.find(Name);
  if (It != Cache.end())
    return It->second;
  sourcekitd_uid_t U = sourcekitd_uid_get_from_buf(Name.data(), Name.size());
  Cache.emplace(Name, U);
  return U;
}

std::string uidName(sourcekitd_uid_t U) {
  if (U == nullptr)
    return {};
  return {sourcekitd_uid_get_string_ptr(U), sourcekitd_uid_get_length(U)};
}

RequestObject toRequest(const json::Value &V, bool AsUID = false) {
  switch (V.kind()) {
  case json::Value::Kind::Null:
    return RequestObject(sourcekitd_request_dictionary_create(nullptr, nullptr, 0));
  case json::Value::Kind::Bool:
    return RequestObject(sourcekitd_request_int64_create(V.asBool() ? 1 : 0));
  case json::Value::Kind::Number:
    return RequestObject(sourcekitd_request_int64_create(V.asInt()));
  case json::Value::Kind::String:
    if (AsUID)
      return RequestObject(sourcekitd_request_uid_create(uid(V.asString())));
    return RequestObject(sourcekitd_request_string_create(V.asString().c_str()));
  case json::Value::Kind::Array: {
    RequestObject A(sourcekitd_request_array_create(nullptr, 0));
    for (const json::Value &E : V.elements()) {
      const RequestObject Element = toRequest(E);
      sourcekitd_request_array_set_value(A.get(), ArrayAppend, Element.get());
    }
    return A;
  }
  case json::Value::Kind::Object: {
    RequestObject D(sourcekitd_request_dictionary_create(nullptr, nullptr, 0));
    for (const json::Member &M : V.members()) {
      const RequestObject Element = toRequest(M.Val, M.Key == "key.request");
      sourcekitd_request_dictionary_set_value(D.get(), uid(M.Key), Element.get());
    }
    return D;
  }
  }
  return RequestObject(sourcekitd_request_dictionary_create(nullptr, nullptr, 0));
}

json::Value toJSON(sourcekitd_variant_t V);

bool addMember(sourcekitd_uid_t Key, sourcekitd_variant_t Value, void *Context) {
  static_cast<json::Value *>(Context)->set(uidName(Key), toJSON(Value));
  return true; // keep going
}

json::Value toJSON(sourcekitd_variant_t V) {
  switch (sourcekitd_variant_get_type(V)) {
  case SOURCEKITD_VARIANT_TYPE_NULL:
    return {};
  case SOURCEKITD_VARIANT_TYPE_DICTIONARY: {
    json::Value D = json::Value::object();
    // The function-pointer applier: the block one needs -fblocks.
    (void)sourcekitd_variant_dictionary_apply_f(V, addMember, &D);
    return D;
  }
  case SOURCEKITD_VARIANT_TYPE_ARRAY: {
    json::Value A = json::Value::array();
    const std::size_t N = sourcekitd_variant_array_get_count(V);
    for (std::size_t I = 0; I < N; ++I)
      A.push(toJSON(sourcekitd_variant_array_get_value(V, I)));
    return A;
  }
  case SOURCEKITD_VARIANT_TYPE_INT64:
    return {sourcekitd_variant_int64_get_value(V)};
  case SOURCEKITD_VARIANT_TYPE_BOOL:
    return {sourcekitd_variant_bool_get_value(V)};
  case SOURCEKITD_VARIANT_TYPE_DOUBLE:
    return {sourcekitd_variant_double_get_value(V)};
  case SOURCEKITD_VARIANT_TYPE_STRING: {
    const char *P = sourcekitd_variant_string_get_ptr(V);
    return {std::string_view(P != nullptr ? P : "", sourcekitd_variant_string_get_length(V))};
  }
  case SOURCEKITD_VARIANT_TYPE_UID:
    return {uidName(sourcekitd_variant_uid_get_value(V))};
  case SOURCEKITD_VARIANT_TYPE_DATA:
    return {}; // no request this server sends answers with raw data
  }
  return {};
}

/// Appends one line to $SOURCEKIT_LSP_LITE_RECORD, if set.
void record(const json::Value &Request, const Reply &R) {
  static const char *const Path =
      std::getenv("SOURCEKIT_LSP_LITE_RECORD"); // NOLINT(concurrency-mt-unsafe): one thread
  if (Path == nullptr || *Path == '\0')
    return;
  json::Value Line = json::Value::object();
  Line.set("request", Request);
  if (R.OK)
    Line.set("response", R.Value);
  else
    Line.set("error", R.Error);
  const std::string Text = json::serialize(Line) + "\n";
  const std::unique_ptr<std::FILE, int (*)(std::FILE *)> F(std::fopen(Path, "ab"), &std::fclose);
  if (F)
    (void)std::fwrite(Text.data(), 1, Text.size(), F.get());
}

} // namespace

Session::Session() { sourcekitd_initialize(); }

Session::~Session() { sourcekitd_shutdown(); }

// Not static, though it could be: a member makes "a Session exists" a
// precondition the compiler checks.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Reply Session::send(const json::Value &Request) const {
  const RequestObject Req = toRequest(Request);
  const ResponseObject Resp(sourcekitd_send_request_sync(Req.get()));
  Reply R;
  if (sourcekitd_response_is_error(Resp.get())) {
    const char *D = sourcekitd_response_error_get_description(Resp.get());
    R.Error = D != nullptr ? D : "unknown sourcekitd error";
  } else {
    R.OK = true;
    R.Value = toJSON(sourcekitd_response_get_value(Resp.get()));
  }
  record(Request, R);
  return R;
}

} // namespace lite::skd
