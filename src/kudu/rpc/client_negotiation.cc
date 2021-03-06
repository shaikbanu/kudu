// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/rpc/client_negotiation.h"

#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sasl/sasl.h>

#include "kudu/gutil/casts.h"
#include "kudu/gutil/endian.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/rpc/blocking_ops.h"
#include "kudu/rpc/constants.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/rpc/sasl_common.h"
#include "kudu/rpc/sasl_helper.h"
#include "kudu/rpc/serialization.h"
#include "kudu/security/cert.h"
#include "kudu/security/tls_context.h"
#include "kudu/security/tls_handshake.h"
#include "kudu/security/tls_socket.h"
#include "kudu/util/faststring.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/net/socket.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/trace.h"

using std::map;
using std::set;
using std::string;
using std::unique_ptr;

using strings::Substitute;

DECLARE_bool(rpc_encrypt_loopback_connections);

namespace kudu {
namespace rpc {

static int ClientNegotiationGetoptCb(ClientNegotiation* client_negotiation,
                                     const char* plugin_name,
                                     const char* option,
                                     const char** result,
                                     unsigned* len) {
  return client_negotiation->GetOptionCb(plugin_name, option, result, len);
}

static int ClientNegotiationSimpleCb(ClientNegotiation* client_negotiation,
                                     int id,
                                     const char** result,
                                     unsigned* len) {
  return client_negotiation->SimpleCb(id, result, len);
}

static int ClientNegotiationSecretCb(sasl_conn_t* conn,
                                     ClientNegotiation* client_negotiation,
                                     int id,
                                     sasl_secret_t** psecret) {
  return client_negotiation->SecretCb(conn, id, psecret);
}

// Return an appropriately-typed Status object based on an ErrorStatusPB returned
// from an Error RPC.
// In case there is no relevant Status type, return a RuntimeError.
static Status StatusFromRpcError(const ErrorStatusPB& error) {
  DCHECK(error.IsInitialized()) << "Error status PB must be initialized";
  if (PREDICT_FALSE(!error.has_code())) {
    return Status::RuntimeError(error.message());
  }
  string code_name = ErrorStatusPB::RpcErrorCodePB_Name(error.code());
  switch (error.code()) {
    case ErrorStatusPB_RpcErrorCodePB_FATAL_UNAUTHORIZED:
      return Status::NotAuthorized(code_name, error.message());
    default:
      return Status::RuntimeError(code_name, error.message());
  }
}

ClientNegotiation::ClientNegotiation(unique_ptr<Socket> socket,
                                     const security::TlsContext* tls_context)
    : socket_(std::move(socket)),
      helper_(SaslHelper::CLIENT),
      tls_context_(tls_context),
      tls_negotiated_(false),
      negotiated_mech_(SaslMechanism::INVALID),
      deadline_(MonoTime::Max()) {
  callbacks_.push_back(SaslBuildCallback(SASL_CB_GETOPT,
      reinterpret_cast<int (*)()>(&ClientNegotiationGetoptCb), this));
  callbacks_.push_back(SaslBuildCallback(SASL_CB_AUTHNAME,
      reinterpret_cast<int (*)()>(&ClientNegotiationSimpleCb), this));
  callbacks_.push_back(SaslBuildCallback(SASL_CB_PASS,
      reinterpret_cast<int (*)()>(&ClientNegotiationSecretCb), this));
  callbacks_.push_back(SaslBuildCallback(SASL_CB_LIST_END, nullptr, nullptr));
  DCHECK(socket_);
  DCHECK(tls_context_);
}

Status ClientNegotiation::EnablePlain(const string& user, const string& pass) {
  RETURN_NOT_OK(helper_.EnablePlain());
  plain_auth_user_ = user;
  plain_pass_ = pass;
  return Status::OK();
}

Status ClientNegotiation::EnableGSSAPI() {
  return helper_.EnableGSSAPI();
}

SaslMechanism::Type ClientNegotiation::negotiated_mechanism() const {
  return negotiated_mech_;
}

void ClientNegotiation::set_server_fqdn(const string& domain_name) {
  helper_.set_server_fqdn(domain_name);
}

void ClientNegotiation::set_deadline(const MonoTime& deadline) {
  deadline_ = deadline;
}

Status ClientNegotiation::Negotiate() {
  TRACE("Beginning negotiation");

  // Ensure we can use blocking calls on the socket during negotiation.
  RETURN_NOT_OK(EnsureBlockingMode(socket_.get()));

  // Step 1: send the connection header.
  RETURN_NOT_OK(SendConnectionHeader());

  faststring recv_buf;

  { // Step 2: send and receive the NEGOTIATE step messages.
    RETURN_NOT_OK(SendNegotiate());
    NegotiatePB response;
    RETURN_NOT_OK(RecvNegotiatePB(&response, &recv_buf));
    RETURN_NOT_OK(HandleNegotiate(response));
  }

  // Step 3: if both ends support TLS, do a TLS handshake.
  // TODO(PKI): allow the client to require TLS.
  if (ContainsKey(server_features_, TLS)) {
    RETURN_NOT_OK(tls_context_->InitiateHandshake(security::TlsHandshakeType::CLIENT,
                                                  &tls_handshake_));

    if (negotiated_mech_ == SaslMechanism::GSSAPI ||
        negotiated_mech_ == SaslMechanism::PLAIN) {
      // When using GSSAPI, we don't verify the server's certificate. Instead,
      // we rely on Kerberos authentication, and use channel binding to tie the
      // SASL authentication to the TLS channel.
      //
      // When using 'PLAIN' authentication, this implies that strong authentication
      // is not enabled. So, we are just using TLS for encryption and don't need to
      // validate a cert.
      tls_handshake_.set_verification_mode(security::TlsVerificationMode::VERIFY_NONE);
    }

    // To initiate the TLS handshake, we pretend as if the server sent us an
    // empty TLS_HANDSHAKE token.
    NegotiatePB initial;
    initial.set_step(NegotiatePB::TLS_HANDSHAKE);
    initial.set_tls_handshake("");
    Status s = HandleTlsHandshake(initial);

    while (s.IsIncomplete()) {
      NegotiatePB response;
      RETURN_NOT_OK(RecvNegotiatePB(&response, &recv_buf));
      s = HandleTlsHandshake(response);
    }
    RETURN_NOT_OK(s);
    tls_negotiated_ = true;
  }

  // Step 4: SASL negotiation.
  RETURN_NOT_OK(InitSaslClient());
  RETURN_NOT_OK(SendSaslInitiate());
  for (bool cont = true; cont; ) {
    NegotiatePB response;
    RETURN_NOT_OK(RecvNegotiatePB(&response, &recv_buf));
    Status s;
    switch (response.step()) {
      // SASL_CHALLENGE: Server sent us a follow-up to an SASL_INITIATE or SASL_RESPONSE request.
      case NegotiatePB::SASL_CHALLENGE:
        RETURN_NOT_OK(HandleSaslChallenge(response));
        break;
      // SASL_SUCCESS: Server has accepted our authentication request. Negotiation successful.
      case NegotiatePB::SASL_SUCCESS:
        RETURN_NOT_OK(HandleSaslSuccess(response));
        cont = false;
        break;
      default:
        return Status::NotAuthorized("expected SASL_CHALLENGE or SASL_SUCCESS step",
                                     NegotiatePB::NegotiateStep_Name(response.step()));
    }
  }

  // Step 5: Send connection context.
  RETURN_NOT_OK(SendConnectionContext());

  TRACE("Negotiation successful");
  return Status::OK();
}

Status ClientNegotiation::SendNegotiatePB(const NegotiatePB& msg) {
  RequestHeader header;
  header.set_call_id(kNegotiateCallId);

  DCHECK(socket_);
  DCHECK(msg.IsInitialized()) << "message must be initialized";
  DCHECK(msg.has_step()) << "message must have a step";

  TRACE("Sending $0 NegotiatePB request", NegotiatePB::NegotiateStep_Name(msg.step()));
  return SendFramedMessageBlocking(socket(), header, msg, deadline_);
}

Status ClientNegotiation::RecvNegotiatePB(NegotiatePB* msg, faststring* buffer) {
  ResponseHeader header;
  Slice param_buf;
  RETURN_NOT_OK(ReceiveFramedMessageBlocking(socket(), buffer, &header, &param_buf, deadline_));
  RETURN_NOT_OK(helper_.CheckNegotiateCallId(header.call_id()));

  if (header.is_error()) {
    return ParseError(param_buf);
  }

  RETURN_NOT_OK(helper_.ParseNegotiatePB(param_buf, msg));
  TRACE("Received $0 NegotiatePB response", NegotiatePB::NegotiateStep_Name(msg->step()));
  return Status::OK();
}

Status ClientNegotiation::ParseError(const Slice& err_data) {
  ErrorStatusPB error;
  if (!error.ParseFromArray(err_data.data(), err_data.size())) {
    return Status::IOError("invalid error response, missing fields",
                           error.InitializationErrorString());
  }
  Status s = StatusFromRpcError(error);
  TRACE("Received error response from server: $0", s.ToString());
  return s;
}

Status ClientNegotiation::SendConnectionHeader() {
  const uint8_t buflen = kMagicNumberLength + kHeaderFlagsLength;
  uint8_t buf[buflen];
  serialization::SerializeConnHeader(buf);
  size_t nsent;
  return socket()->BlockingWrite(buf, buflen, &nsent, deadline_);
}

Status ClientNegotiation::InitSaslClient() {
  RETURN_NOT_OK(SaslInit());

  // TODO(unknown): Support security flags.
  unsigned secflags = 0;

  sasl_conn_t* sasl_conn = nullptr;
  RETURN_NOT_OK_PREPEND(WrapSaslCall(nullptr /* no conn */, [&]() {
      return sasl_client_new(
          kSaslProtoName,               // Registered name of the service using SASL. Required.
          helper_.server_fqdn(),        // The fully qualified domain name of the remote server.
          nullptr,                      // Local and remote IP address strings. (we don't use
          nullptr,                      // any mechanisms which require this info.)
          &callbacks_[0],               // Connection-specific callbacks.
          secflags,                     // Security flags.
          &sasl_conn);
    }), "Unable to create new SASL client");
  sasl_conn_.reset(sasl_conn);
  return Status::OK();
}

Status ClientNegotiation::SendNegotiate() {
  NegotiatePB msg;
  msg.set_step(NegotiatePB::NEGOTIATE);

  // Advertise our supported features.
  client_features_ = kSupportedClientRpcFeatureFlags;
  // If the remote peer is local, then we allow using TLS for authentication
  // without encryption or integrity.
  if (socket_->IsLoopbackConnection() && !FLAGS_rpc_encrypt_loopback_connections) {
    client_features_.insert(TLS_AUTHENTICATION_ONLY);
  }

  for (RpcFeatureFlag feature : client_features_) {
    msg.add_supported_features(feature);
  }

  RETURN_NOT_OK(SendNegotiatePB(msg));
  return Status::OK();
}

Status ClientNegotiation::HandleNegotiate(const NegotiatePB& response) {
  if (PREDICT_FALSE(response.step() != NegotiatePB::NEGOTIATE)) {
    return Status::NotAuthorized("expected NEGOTIATE step",
                                 NegotiatePB::NegotiateStep_Name(response.step()));
  }
  TRACE("Received NEGOTIATE response from server");

  // Fill in the set of features supported by the server.
  for (int flag : response.supported_features()) {
    // We only add the features that our local build knows about.
    RpcFeatureFlag feature_flag = RpcFeatureFlag_IsValid(flag) ?
                                  static_cast<RpcFeatureFlag>(flag) : UNKNOWN;
    if (feature_flag != UNKNOWN) {
      server_features_.insert(feature_flag);
    }
  }

  // Build a map of the SASL mechanisms offered by the server.
  const set<SaslMechanism::Type>& client_mechs = helper_.EnabledMechs();
  set<SaslMechanism::Type> server_mechs;
  for (const NegotiatePB::SaslMechanism& sasl_mech : response.sasl_mechanisms()) {
    auto mech = SaslMechanism::value_of(sasl_mech.mechanism());
    if (mech == SaslMechanism::INVALID) {
      continue;
    }
    server_mechs.insert(mech);
  }

  // Determine which SASL mechanism to use for authenticating the connection.
  // We pick the most preferred mechanism which is supported by both parties.
  // The preference list in order of most to least preferred:
  //  * GSSAPI
  //  * PLAIN
  set<SaslMechanism::Type> common_mechs = STLSetIntersection(client_mechs, server_mechs);

  if (common_mechs.empty()) {
    if (ContainsKey(server_mechs, SaslMechanism::GSSAPI) &&
        !ContainsKey(client_mechs, SaslMechanism::GSSAPI)) {
      return Status::NotAuthorized("server requires authentication, "
                                   "but client does not have Kerberos enabled");
    }
    if (!ContainsKey(server_mechs, SaslMechanism::GSSAPI) &&
        ContainsKey(client_mechs, SaslMechanism::GSSAPI)) {
      return Status::NotAuthorized("client requires authentication, "
                                   "but server does not have Kerberos enabled");
    }
    string msg = Substitute("client/server supported SASL mechanism mismatch; "
                            "client mechanisms: [$0], server mechanisms: [$1]",
                            JoinMapped(client_mechs, SaslMechanism::name_of, ", "),
                            JoinMapped(server_mechs, SaslMechanism::name_of, ", "));

    // For now, there should never be a SASL mechanism mismatch that isn't due
    // to one of the sides requiring Kerberos and the other not having it, so
    // lets sanity check that.
    DLOG(FATAL) << msg;
    return Status::NotAuthorized(msg);
  }

  // TODO(PKI): allow the client to require authentication.
  if (ContainsKey(common_mechs, SaslMechanism::GSSAPI)) {
    negotiated_mech_ = SaslMechanism::GSSAPI;
  } else {
    DCHECK(ContainsKey(common_mechs, SaslMechanism::PLAIN));
    negotiated_mech_ = SaslMechanism::PLAIN;
  }

  return Status::OK();
}

Status ClientNegotiation::SendTlsHandshake(string tls_token) {
  TRACE("Sending TLS_HANDSHAKE message to server");
  NegotiatePB msg;
  msg.set_step(NegotiatePB::TLS_HANDSHAKE);
  msg.mutable_tls_handshake()->swap(tls_token);
  return SendNegotiatePB(msg);
}

Status ClientNegotiation::HandleTlsHandshake(const NegotiatePB& response) {
  if (PREDICT_FALSE(response.step() != NegotiatePB::TLS_HANDSHAKE)) {
    return Status::NotAuthorized("expected TLS_HANDSHAKE step",
                                 NegotiatePB::NegotiateStep_Name(response.step()));
  }
  TRACE("Received TLS_HANDSHAKE response from server");

  if (PREDICT_FALSE(!response.has_tls_handshake())) {
    return Status::NotAuthorized("No TLS handshake token in TLS_HANDSHAKE response from server");
  }

  string token;
  Status s = tls_handshake_.Continue(response.tls_handshake(), &token);
  if (s.IsIncomplete()) {
    // Another roundtrip is required to complete the handshake.
    RETURN_NOT_OK(SendTlsHandshake(std::move(token)));
  }

  // Check that the handshake step didn't produce an error. Will also propagate
  // an Incomplete status.
  RETURN_NOT_OK(s);

  // TLS handshake is finished.
  DCHECK(token.empty());

  if (ContainsKey(server_features_, TLS_AUTHENTICATION_ONLY) &&
      ContainsKey(client_features_, TLS_AUTHENTICATION_ONLY)) {
    TRACE("Negotiated auth-only TLS");
    return tls_handshake_.FinishNoWrap(*socket_);
  }
  return tls_handshake_.Finish(&socket_);
}

Status ClientNegotiation::SendSaslInitiate() {
  TRACE("Initiating SASL $0 handshake", negotiated_mech_);

  // At this point we've already chosen the SASL mechanism to use
  // (negotiated_mech_), but we need to let the SASL library know. SASL likes to
  // choose the mechanism from among a list of possible options, so we simply
  // provide it one option, and then check that it picks that option.

  const char* init_msg = nullptr;
  unsigned init_msg_len = 0;
  const char* negotiated_mech = nullptr;

  /* select a mechanism for a connection
   *  mechlist      -- mechanisms server has available (punctuation ignored)
   * output:
   *  prompt_need   -- on SASL_INTERACT, list of prompts needed to continue
   *  clientout     -- the initial client response to send to the server
   *  mech          -- set to mechanism name
   *
   * Returns:
   *  SASL_OK       -- success
   *  SASL_CONTINUE -- negotiation required
   *  SASL_NOMEM    -- not enough memory
   *  SASL_NOMECH   -- no mechanism meets requested properties
   *  SASL_INTERACT -- user interaction needed to fill in prompt_need list
   */
  TRACE("Calling sasl_client_start()");
  Status s = WrapSaslCall(sasl_conn_.get(), [&]() {
      return sasl_client_start(
          sasl_conn_.get(),                         // The SASL connection context created by init()
          SaslMechanism::name_of(negotiated_mech_), // The list of mechanisms to negotiate.
          nullptr,                                  // Disables INTERACT return if NULL.
          &init_msg,                                // Filled in on success.
          &init_msg_len,                            // Filled in on success.
          &negotiated_mech);                        // Filled in on success.
  });

  if (PREDICT_FALSE(!s.IsIncomplete() && !s.ok())) {
    return s;
  }

  // Check that the SASL library is using the mechanism that we picked.
  DCHECK_EQ(SaslMechanism::value_of(negotiated_mech), negotiated_mech_);

  // If we are speaking TLS and the negotiated mechanism is GSSAPI (Kerberos),
  // configure SASL to use integrity protection so that the channel bindings
  // can be verified.
  if (tls_negotiated_ && negotiated_mech_ == SaslMechanism::GSSAPI) {
    RETURN_NOT_OK(EnableIntegrityProtection(sasl_conn_.get()));
  }

  NegotiatePB msg;
  msg.set_step(NegotiatePB::SASL_INITIATE);
  msg.mutable_token()->assign(init_msg, init_msg_len);
  msg.add_sasl_mechanisms()->set_mechanism(negotiated_mech);
  return SendNegotiatePB(msg);
}

Status ClientNegotiation::SendSaslResponse(const char* resp_msg, unsigned resp_msg_len) {
  NegotiatePB reply;
  reply.set_step(NegotiatePB::SASL_RESPONSE);
  reply.mutable_token()->assign(resp_msg, resp_msg_len);
  return SendNegotiatePB(reply);
}

Status ClientNegotiation::HandleSaslChallenge(const NegotiatePB& response) {
  TRACE("Received SASL_CHALLENGE response from server");
  if (PREDICT_FALSE(!response.has_token())) {
    return Status::NotAuthorized("no token in SASL_CHALLENGE response from server");
  }

  const char* out = nullptr;
  unsigned out_len = 0;
  Status s = DoSaslStep(response.token(), &out, &out_len);
  if (PREDICT_FALSE(!s.IsIncomplete() && !s.ok())) {
    return s;
  }

  return SendSaslResponse(out, out_len);
}

Status ClientNegotiation::HandleSaslSuccess(const NegotiatePB& response) {
  TRACE("Received SASL_SUCCESS response from server");

  if (tls_negotiated_ && negotiated_mech_ == SaslMechanism::Type::GSSAPI) {
    // Check the channel bindings provided by the server against the expected
    // channel bindings.
    security::Cert cert;
    RETURN_NOT_OK(tls_handshake_.GetRemoteCert(&cert));

    string expected_channel_bindings;
    RETURN_NOT_OK_PREPEND(cert.GetServerEndPointChannelBindings(&expected_channel_bindings),
                          "failed to generate channel bindings");

    if (!response.has_channel_bindings()) {
      return Status::NotAuthorized("no channel bindings provided by server");
    }

    string recieved_channel_bindings;
    RETURN_NOT_OK_PREPEND(SaslDecode(response.channel_bindings(), &recieved_channel_bindings),
                          "failed to decode channel bindings");

    if (expected_channel_bindings != recieved_channel_bindings) {
      Sockaddr addr;
      ignore_result(socket_->GetPeerAddress(&addr));

      LOG(WARNING) << "Recieved unexpected channel bindings from server "
                   << addr.ToString()
                   << ", this could indicate an active network man-in-the-middle";
      return Status::NotAuthorized("channel bindings do not match");
    }
  }

  return Status::OK();
}

Status ClientNegotiation::DoSaslStep(const string& in, const char** out, unsigned* out_len) {
  TRACE("Calling sasl_client_step()");

  return WrapSaslCall(sasl_conn_.get(), [&]() {
      return sasl_client_step(sasl_conn_.get(), in.c_str(), in.length(), nullptr, out, out_len);
  });
}

Status ClientNegotiation::SaslDecode(const string& encoded, string* plaintext) {
  size_t offset = 0;
  const char* out;
  unsigned out_len;

  // The SASL library can only decode up to a maximum amount at a time, so we
  // have to call decode multiple times if our input is larger than this max.
  while (offset < encoded.size()) {
    size_t len = std::min(kSaslMaxOutBufLen, encoded.size() - offset);

    RETURN_NOT_OK(WrapSaslCall(sasl_conn_.get(), [&]() {
        return sasl_decode(sasl_conn_.get(), encoded.data() + offset, len, &out, &out_len);
    }));

    plaintext->append(out, out_len);
    offset += len;
  }

  return Status::OK();
}

Status ClientNegotiation::SendConnectionContext() {
  TRACE("Sending connection context");
  RequestHeader header;
  header.set_call_id(kConnectionContextCallId);

  ConnectionContextPB conn_context;
  // This field is deprecated but used by servers <Kudu 1.1. Newer server versions ignore
  // this and use the SASL-provided username instead.
  conn_context.mutable_deprecated_user_info()->set_real_user(
      plain_auth_user_.empty() ? "cpp-client" : plain_auth_user_);
  return SendFramedMessageBlocking(socket(), header, conn_context, deadline_);
}

int ClientNegotiation::GetOptionCb(const char* plugin_name, const char* option,
                            const char** result, unsigned* len) {
  return helper_.GetOptionCb(plugin_name, option, result, len);
}

// Used for PLAIN.
// SASL callback for SASL_CB_USER, SASL_CB_AUTHNAME, SASL_CB_LANGUAGE
int ClientNegotiation::SimpleCb(int id, const char** result, unsigned* len) {
  if (PREDICT_FALSE(!helper_.IsPlainEnabled())) {
    LOG(DFATAL) << "Simple callback called, but PLAIN auth is not enabled";
    return SASL_FAIL;
  }
  if (PREDICT_FALSE(result == nullptr)) {
    LOG(DFATAL) << "result outparam is NULL";
    return SASL_BADPARAM;
  }
  switch (id) {
    // TODO(unknown): Support impersonation?
    // For impersonation, USER is the impersonated user, AUTHNAME is the "sudoer".
    case SASL_CB_USER:
      TRACE("callback for SASL_CB_USER");
      *result = plain_auth_user_.c_str();
      if (len != nullptr) *len = plain_auth_user_.length();
      break;
    case SASL_CB_AUTHNAME:
      TRACE("callback for SASL_CB_AUTHNAME");
      *result = plain_auth_user_.c_str();
      if (len != nullptr) *len = plain_auth_user_.length();
      break;
    case SASL_CB_LANGUAGE:
      LOG(DFATAL) << "Unable to handle SASL callback type SASL_CB_LANGUAGE"
        << "(" << id << ")";
      return SASL_BADPARAM;
    default:
      LOG(DFATAL) << "Unexpected SASL callback type: " << id;
      return SASL_BADPARAM;
  }

  return SASL_OK;
}

// Used for PLAIN.
// SASL callback for SASL_CB_PASS: User password.
int ClientNegotiation::SecretCb(sasl_conn_t* conn, int id, sasl_secret_t** psecret) {
  if (PREDICT_FALSE(!helper_.IsPlainEnabled())) {
    LOG(DFATAL) << "Plain secret callback called, but PLAIN auth is not enabled";
    return SASL_FAIL;
  }
  switch (id) {
    case SASL_CB_PASS: {
      if (!conn || !psecret) return SASL_BADPARAM;

      size_t len = plain_pass_.length();
      *psecret = reinterpret_cast<sasl_secret_t*>(malloc(sizeof(sasl_secret_t) + len));
      if (!*psecret) {
        return SASL_NOMEM;
      }
      psecret_.reset(*psecret);  // Ensure that we free() this structure later.
      (*psecret)->len = len;
      memcpy((*psecret)->data, plain_pass_.c_str(), len + 1);
      break;
    }
    default:
      LOG(DFATAL) << "Unexpected SASL callback type: " << id;
      return SASL_BADPARAM;
  }

  return SASL_OK;
}

} // namespace rpc
} // namespace kudu
