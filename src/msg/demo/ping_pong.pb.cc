// Generated by the protocol buffer compiler.  DO NOT EDIT!
// source: ping_pong.proto

#include "ping_pong.pb.h"

#include <algorithm>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/extension_set.h>
#include <google/protobuf/wire_format_lite.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/generated_message_reflection.h>
#include <google/protobuf/reflection_ops.h>
#include <google/protobuf/wire_format.h>
// @@protoc_insertion_point(includes)
#include <google/protobuf/port_def.inc>
namespace ping_pong {
class requestDefaultTypeInternal {
 public:
  ::PROTOBUF_NAMESPACE_ID::internal::ExplicitlyConstructed<request> _instance;
} _request_default_instance_;
class responseDefaultTypeInternal {
 public:
  ::PROTOBUF_NAMESPACE_ID::internal::ExplicitlyConstructed<response> _instance;
} _response_default_instance_;
}  // namespace ping_pong
static void InitDefaultsscc_info_request_ping_5fpong_2eproto() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  {
    void* ptr = &::ping_pong::_request_default_instance_;
    new (ptr) ::ping_pong::request();
    ::PROTOBUF_NAMESPACE_ID::internal::OnShutdownDestroyMessage(ptr);
  }
  ::ping_pong::request::InitAsDefaultInstance();
}

::PROTOBUF_NAMESPACE_ID::internal::SCCInfo<0> scc_info_request_ping_5fpong_2eproto =
    {{ATOMIC_VAR_INIT(::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase::kUninitialized), 0, 0, InitDefaultsscc_info_request_ping_5fpong_2eproto}, {}};

static void InitDefaultsscc_info_response_ping_5fpong_2eproto() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  {
    void* ptr = &::ping_pong::_response_default_instance_;
    new (ptr) ::ping_pong::response();
    ::PROTOBUF_NAMESPACE_ID::internal::OnShutdownDestroyMessage(ptr);
  }
  ::ping_pong::response::InitAsDefaultInstance();
}

::PROTOBUF_NAMESPACE_ID::internal::SCCInfo<0> scc_info_response_ping_5fpong_2eproto =
    {{ATOMIC_VAR_INIT(::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase::kUninitialized), 0, 0, InitDefaultsscc_info_response_ping_5fpong_2eproto}, {}};

static ::PROTOBUF_NAMESPACE_ID::Metadata file_level_metadata_ping_5fpong_2eproto[2];
static constexpr ::PROTOBUF_NAMESPACE_ID::EnumDescriptor const** file_level_enum_descriptors_ping_5fpong_2eproto = nullptr;
static const ::PROTOBUF_NAMESPACE_ID::ServiceDescriptor* file_level_service_descriptors_ping_5fpong_2eproto[1];

const ::PROTOBUF_NAMESPACE_ID::uint32 TableStruct_ping_5fpong_2eproto::offsets[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  PROTOBUF_FIELD_OFFSET(::ping_pong::request, _has_bits_),
  PROTOBUF_FIELD_OFFSET(::ping_pong::request, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  PROTOBUF_FIELD_OFFSET(::ping_pong::request, ping_),
  0,
  PROTOBUF_FIELD_OFFSET(::ping_pong::response, _has_bits_),
  PROTOBUF_FIELD_OFFSET(::ping_pong::response, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  PROTOBUF_FIELD_OFFSET(::ping_pong::response, pong_),
  0,
};
static const ::PROTOBUF_NAMESPACE_ID::internal::MigrationSchema schemas[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  { 0, 6, sizeof(::ping_pong::request)},
  { 7, 13, sizeof(::ping_pong::response)},
};

static ::PROTOBUF_NAMESPACE_ID::Message const * const file_default_instances[] = {
  reinterpret_cast<const ::PROTOBUF_NAMESPACE_ID::Message*>(&::ping_pong::_request_default_instance_),
  reinterpret_cast<const ::PROTOBUF_NAMESPACE_ID::Message*>(&::ping_pong::_response_default_instance_),
};

const char descriptor_table_protodef_ping_5fpong_2eproto[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) =
  "\n\017ping_pong.proto\022\tping_pong\"\027\n\007request\022"
  "\014\n\004ping\030\001 \002(\t\"\030\n\010response\022\014\n\004pong\030\002 \002(\t2"
  "\177\n\021ping_pong_service\0224\n\tping_pong\022\022.ping"
  "_pong.request\032\023.ping_pong.response\0224\n\the"
  "artbeat\022\022.ping_pong.request\032\023.ping_pong."
  "responseB\003\200\001\001"
  ;
static const ::PROTOBUF_NAMESPACE_ID::internal::DescriptorTable*const descriptor_table_ping_5fpong_2eproto_deps[1] = {
};
static ::PROTOBUF_NAMESPACE_ID::internal::SCCInfoBase*const descriptor_table_ping_5fpong_2eproto_sccs[2] = {
  &scc_info_request_ping_5fpong_2eproto.base,
  &scc_info_response_ping_5fpong_2eproto.base,
};
static ::PROTOBUF_NAMESPACE_ID::internal::once_flag descriptor_table_ping_5fpong_2eproto_once;
const ::PROTOBUF_NAMESPACE_ID::internal::DescriptorTable descriptor_table_ping_5fpong_2eproto = {
  false, false, descriptor_table_protodef_ping_5fpong_2eproto, "ping_pong.proto", 213,
  &descriptor_table_ping_5fpong_2eproto_once, descriptor_table_ping_5fpong_2eproto_sccs, descriptor_table_ping_5fpong_2eproto_deps, 2, 0,
  schemas, file_default_instances, TableStruct_ping_5fpong_2eproto::offsets,
  file_level_metadata_ping_5fpong_2eproto, 2, file_level_enum_descriptors_ping_5fpong_2eproto, file_level_service_descriptors_ping_5fpong_2eproto,
};

// Force running AddDescriptors() at dynamic initialization time.
static bool dynamic_init_dummy_ping_5fpong_2eproto = (static_cast<void>(::PROTOBUF_NAMESPACE_ID::internal::AddDescriptors(&descriptor_table_ping_5fpong_2eproto)), true);
namespace ping_pong {

// ===================================================================

void request::InitAsDefaultInstance() {
}
class request::_Internal {
 public:
  using HasBits = decltype(std::declval<request>()._has_bits_);
  static void set_has_ping(HasBits* has_bits) {
    (*has_bits)[0] |= 1u;
  }
  static bool MissingRequiredFields(const HasBits& has_bits) {
    return ((has_bits[0] & 0x00000001) ^ 0x00000001) != 0;
  }
};

request::request(::PROTOBUF_NAMESPACE_ID::Arena* arena)
  : ::PROTOBUF_NAMESPACE_ID::Message(arena) {
  SharedCtor();
  RegisterArenaDtor(arena);
  // @@protoc_insertion_point(arena_constructor:ping_pong.request)
}
request::request(const request& from)
  : ::PROTOBUF_NAMESPACE_ID::Message(),
      _has_bits_(from._has_bits_) {
  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  ping_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  if (from._internal_has_ping()) {
    ping_.Set(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), from._internal_ping(),
      GetArena());
  }
  // @@protoc_insertion_point(copy_constructor:ping_pong.request)
}

void request::SharedCtor() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&scc_info_request_ping_5fpong_2eproto.base);
  ping_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

request::~request() {
  // @@protoc_insertion_point(destructor:ping_pong.request)
  SharedDtor();
  _internal_metadata_.Delete<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

void request::SharedDtor() {
  GOOGLE_DCHECK(GetArena() == nullptr);
  ping_.DestroyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

void request::ArenaDtor(void* object) {
  request* _this = reinterpret_cast< request* >(object);
  (void)_this;
}
void request::RegisterArenaDtor(::PROTOBUF_NAMESPACE_ID::Arena*) {
}
void request::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}
const request& request::default_instance() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&::scc_info_request_ping_5fpong_2eproto.base);
  return *internal_default_instance();
}


void request::Clear() {
// @@protoc_insertion_point(message_clear_start:ping_pong.request)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000001u) {
    ping_.ClearNonDefaultToEmpty();
  }
  _has_bits_.Clear();
  _internal_metadata_.Clear<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

const char* request::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  _Internal::HasBits has_bits{};
  ::PROTOBUF_NAMESPACE_ID::Arena* arena = GetArena(); (void)arena;
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      // required string ping = 1;
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 10)) {
          auto str = _internal_mutable_ping();
          ptr = ::PROTOBUF_NAMESPACE_ID::internal::InlineGreedyStringParser(str, ptr, ctx);
          #ifndef NDEBUG
          ::PROTOBUF_NAMESPACE_ID::internal::VerifyUTF8(str, "ping_pong.request.ping");
          #endif  // !NDEBUG
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag,
            _internal_metadata_.mutable_unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(),
            ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  _has_bits_.Or(has_bits);
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* request::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:ping_pong.request)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  // required string ping = 1;
  if (cached_has_bits & 0x00000001u) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::VerifyUTF8StringNamedField(
      this->_internal_ping().data(), static_cast<int>(this->_internal_ping().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::SERIALIZE,
      "ping_pong.request.ping");
    target = stream->WriteStringMaybeAliased(
        1, this->_internal_ping(), target);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(::PROTOBUF_NAMESPACE_ID::UnknownFieldSet::default_instance), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:ping_pong.request)
  return target;
}

size_t request::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:ping_pong.request)
  size_t total_size = 0;

  // required string ping = 1;
  if (_internal_has_ping()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_ping());
  }
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    return ::PROTOBUF_NAMESPACE_ID::internal::ComputeUnknownFieldsSize(
        _internal_metadata_, total_size, &_cached_size_);
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void request::MergeFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_merge_from_start:ping_pong.request)
  GOOGLE_DCHECK_NE(&from, this);
  const request* source =
      ::PROTOBUF_NAMESPACE_ID::DynamicCastToGenerated<request>(
          &from);
  if (source == nullptr) {
  // @@protoc_insertion_point(generalized_merge_from_cast_fail:ping_pong.request)
    ::PROTOBUF_NAMESPACE_ID::internal::ReflectionOps::Merge(from, this);
  } else {
  // @@protoc_insertion_point(generalized_merge_from_cast_success:ping_pong.request)
    MergeFrom(*source);
  }
}

void request::MergeFrom(const request& from) {
// @@protoc_insertion_point(class_specific_merge_from_start:ping_pong.request)
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  if (from._internal_has_ping()) {
    _internal_set_ping(from._internal_ping());
  }
}

void request::CopyFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_copy_from_start:ping_pong.request)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

void request::CopyFrom(const request& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:ping_pong.request)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool request::IsInitialized() const {
  if (_Internal::MissingRequiredFields(_has_bits_)) return false;
  return true;
}

void request::InternalSwap(request* other) {
  using std::swap;
  _internal_metadata_.Swap<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(&other->_internal_metadata_);
  swap(_has_bits_[0], other->_has_bits_[0]);
  ping_.Swap(&other->ping_, &::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), GetArena());
}

::PROTOBUF_NAMESPACE_ID::Metadata request::GetMetadata() const {
  return GetMetadataStatic();
}


// ===================================================================

void response::InitAsDefaultInstance() {
}
class response::_Internal {
 public:
  using HasBits = decltype(std::declval<response>()._has_bits_);
  static void set_has_pong(HasBits* has_bits) {
    (*has_bits)[0] |= 1u;
  }
  static bool MissingRequiredFields(const HasBits& has_bits) {
    return ((has_bits[0] & 0x00000001) ^ 0x00000001) != 0;
  }
};

response::response(::PROTOBUF_NAMESPACE_ID::Arena* arena)
  : ::PROTOBUF_NAMESPACE_ID::Message(arena) {
  SharedCtor();
  RegisterArenaDtor(arena);
  // @@protoc_insertion_point(arena_constructor:ping_pong.response)
}
response::response(const response& from)
  : ::PROTOBUF_NAMESPACE_ID::Message(),
      _has_bits_(from._has_bits_) {
  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  pong_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  if (from._internal_has_pong()) {
    pong_.Set(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), from._internal_pong(),
      GetArena());
  }
  // @@protoc_insertion_point(copy_constructor:ping_pong.response)
}

void response::SharedCtor() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&scc_info_response_ping_5fpong_2eproto.base);
  pong_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

response::~response() {
  // @@protoc_insertion_point(destructor:ping_pong.response)
  SharedDtor();
  _internal_metadata_.Delete<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

void response::SharedDtor() {
  GOOGLE_DCHECK(GetArena() == nullptr);
  pong_.DestroyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

void response::ArenaDtor(void* object) {
  response* _this = reinterpret_cast< response* >(object);
  (void)_this;
}
void response::RegisterArenaDtor(::PROTOBUF_NAMESPACE_ID::Arena*) {
}
void response::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}
const response& response::default_instance() {
  ::PROTOBUF_NAMESPACE_ID::internal::InitSCC(&::scc_info_response_ping_5fpong_2eproto.base);
  return *internal_default_instance();
}


void response::Clear() {
// @@protoc_insertion_point(message_clear_start:ping_pong.response)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000001u) {
    pong_.ClearNonDefaultToEmpty();
  }
  _has_bits_.Clear();
  _internal_metadata_.Clear<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

const char* response::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  _Internal::HasBits has_bits{};
  ::PROTOBUF_NAMESPACE_ID::Arena* arena = GetArena(); (void)arena;
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      // required string pong = 2;
      case 2:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 18)) {
          auto str = _internal_mutable_pong();
          ptr = ::PROTOBUF_NAMESPACE_ID::internal::InlineGreedyStringParser(str, ptr, ctx);
          #ifndef NDEBUG
          ::PROTOBUF_NAMESPACE_ID::internal::VerifyUTF8(str, "ping_pong.response.pong");
          #endif  // !NDEBUG
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag,
            _internal_metadata_.mutable_unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(),
            ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  _has_bits_.Or(has_bits);
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* response::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:ping_pong.response)
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  // required string pong = 2;
  if (cached_has_bits & 0x00000001u) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::VerifyUTF8StringNamedField(
      this->_internal_pong().data(), static_cast<int>(this->_internal_pong().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::SERIALIZE,
      "ping_pong.response.pong");
    target = stream->WriteStringMaybeAliased(
        2, this->_internal_pong(), target);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(::PROTOBUF_NAMESPACE_ID::UnknownFieldSet::default_instance), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:ping_pong.response)
  return target;
}

size_t response::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:ping_pong.response)
  size_t total_size = 0;

  // required string pong = 2;
  if (_internal_has_pong()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_pong());
  }
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    return ::PROTOBUF_NAMESPACE_ID::internal::ComputeUnknownFieldsSize(
        _internal_metadata_, total_size, &_cached_size_);
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void response::MergeFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_merge_from_start:ping_pong.response)
  GOOGLE_DCHECK_NE(&from, this);
  const response* source =
      ::PROTOBUF_NAMESPACE_ID::DynamicCastToGenerated<response>(
          &from);
  if (source == nullptr) {
  // @@protoc_insertion_point(generalized_merge_from_cast_fail:ping_pong.response)
    ::PROTOBUF_NAMESPACE_ID::internal::ReflectionOps::Merge(from, this);
  } else {
  // @@protoc_insertion_point(generalized_merge_from_cast_success:ping_pong.response)
    MergeFrom(*source);
  }
}

void response::MergeFrom(const response& from) {
// @@protoc_insertion_point(class_specific_merge_from_start:ping_pong.response)
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  if (from._internal_has_pong()) {
    _internal_set_pong(from._internal_pong());
  }
}

void response::CopyFrom(const ::PROTOBUF_NAMESPACE_ID::Message& from) {
// @@protoc_insertion_point(generalized_copy_from_start:ping_pong.response)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

void response::CopyFrom(const response& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:ping_pong.response)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool response::IsInitialized() const {
  if (_Internal::MissingRequiredFields(_has_bits_)) return false;
  return true;
}

void response::InternalSwap(response* other) {
  using std::swap;
  _internal_metadata_.Swap<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(&other->_internal_metadata_);
  swap(_has_bits_[0], other->_has_bits_[0]);
  pong_.Swap(&other->pong_, &::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), GetArena());
}

::PROTOBUF_NAMESPACE_ID::Metadata response::GetMetadata() const {
  return GetMetadataStatic();
}


// ===================================================================

ping_pong_service::~ping_pong_service() {}

const ::PROTOBUF_NAMESPACE_ID::ServiceDescriptor* ping_pong_service::descriptor() {
  ::PROTOBUF_NAMESPACE_ID::internal::AssignDescriptors(&descriptor_table_ping_5fpong_2eproto);
  return file_level_service_descriptors_ping_5fpong_2eproto[0];
}

const ::PROTOBUF_NAMESPACE_ID::ServiceDescriptor* ping_pong_service::GetDescriptor() {
  return descriptor();
}

void ping_pong_service::ping_pong(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                         const ::ping_pong::request*,
                         ::ping_pong::response*,
                         ::google::protobuf::Closure* done) {
  controller->SetFailed("Method ping_pong() not implemented.");
  done->Run();
}

void ping_pong_service::heartbeat(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                         const ::ping_pong::request*,
                         ::ping_pong::response*,
                         ::google::protobuf::Closure* done) {
  controller->SetFailed("Method heartbeat() not implemented.");
  done->Run();
}

void ping_pong_service::CallMethod(const ::PROTOBUF_NAMESPACE_ID::MethodDescriptor* method,
                             ::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                             const ::PROTOBUF_NAMESPACE_ID::Message* request,
                             ::PROTOBUF_NAMESPACE_ID::Message* response,
                             ::google::protobuf::Closure* done) {
  GOOGLE_DCHECK_EQ(method->service(), file_level_service_descriptors_ping_5fpong_2eproto[0]);
  switch(method->index()) {
    case 0:
      ping_pong(controller,
             ::PROTOBUF_NAMESPACE_ID::internal::DownCast<const ::ping_pong::request*>(
                 request),
             ::PROTOBUF_NAMESPACE_ID::internal::DownCast<::ping_pong::response*>(
                 response),
             done);
      break;
    case 1:
      heartbeat(controller,
             ::PROTOBUF_NAMESPACE_ID::internal::DownCast<const ::ping_pong::request*>(
                 request),
             ::PROTOBUF_NAMESPACE_ID::internal::DownCast<::ping_pong::response*>(
                 response),
             done);
      break;
    default:
      GOOGLE_LOG(FATAL) << "Bad method index; this should never happen.";
      break;
  }
}

const ::PROTOBUF_NAMESPACE_ID::Message& ping_pong_service::GetRequestPrototype(
    const ::PROTOBUF_NAMESPACE_ID::MethodDescriptor* method) const {
  GOOGLE_DCHECK_EQ(method->service(), descriptor());
  switch(method->index()) {
    case 0:
      return ::ping_pong::request::default_instance();
    case 1:
      return ::ping_pong::request::default_instance();
    default:
      GOOGLE_LOG(FATAL) << "Bad method index; this should never happen.";
      return *::PROTOBUF_NAMESPACE_ID::MessageFactory::generated_factory()
          ->GetPrototype(method->input_type());
  }
}

const ::PROTOBUF_NAMESPACE_ID::Message& ping_pong_service::GetResponsePrototype(
    const ::PROTOBUF_NAMESPACE_ID::MethodDescriptor* method) const {
  GOOGLE_DCHECK_EQ(method->service(), descriptor());
  switch(method->index()) {
    case 0:
      return ::ping_pong::response::default_instance();
    case 1:
      return ::ping_pong::response::default_instance();
    default:
      GOOGLE_LOG(FATAL) << "Bad method index; this should never happen.";
      return *::PROTOBUF_NAMESPACE_ID::MessageFactory::generated_factory()
          ->GetPrototype(method->output_type());
  }
}

ping_pong_service_Stub::ping_pong_service_Stub(::PROTOBUF_NAMESPACE_ID::RpcChannel* channel)
  : channel_(channel), owns_channel_(false) {}
ping_pong_service_Stub::ping_pong_service_Stub(
    ::PROTOBUF_NAMESPACE_ID::RpcChannel* channel,
    ::PROTOBUF_NAMESPACE_ID::Service::ChannelOwnership ownership)
  : channel_(channel),
    owns_channel_(ownership == ::PROTOBUF_NAMESPACE_ID::Service::STUB_OWNS_CHANNEL) {}
ping_pong_service_Stub::~ping_pong_service_Stub() {
  if (owns_channel_) delete channel_;
}

void ping_pong_service_Stub::ping_pong(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                              const ::ping_pong::request* request,
                              ::ping_pong::response* response,
                              ::google::protobuf::Closure* done) {
  channel_->CallMethod(descriptor()->method(0),
                       controller, request, response, done);
}
void ping_pong_service_Stub::heartbeat(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                              const ::ping_pong::request* request,
                              ::ping_pong::response* response,
                              ::google::protobuf::Closure* done) {
  channel_->CallMethod(descriptor()->method(1),
                       controller, request, response, done);
}

// @@protoc_insertion_point(namespace_scope)
}  // namespace ping_pong
PROTOBUF_NAMESPACE_OPEN
template<> PROTOBUF_NOINLINE ::ping_pong::request* Arena::CreateMaybeMessage< ::ping_pong::request >(Arena* arena) {
  return Arena::CreateMessageInternal< ::ping_pong::request >(arena);
}
template<> PROTOBUF_NOINLINE ::ping_pong::response* Arena::CreateMaybeMessage< ::ping_pong::response >(Arena* arena) {
  return Arena::CreateMessageInternal< ::ping_pong::response >(arena);
}
PROTOBUF_NAMESPACE_CLOSE

// @@protoc_insertion_point(global_scope)
#include <google/protobuf/port_undef.inc>
