// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"
#include "ds/files.h"
#include "ds/logger.h"
#include "enclave/appinterface.h"
#include "node/clientsignatures.h"
#include "node/encryptor.h"
#include "node/genesisgen.h"
#include "node/rpc/jsonrpc.h"
#include "node/rpc/memberfrontend.h"
#include "node/rpc/userfrontend.h"
#include "node_stub.h"
#include "runtime_config/default_whitelists.h"

#include <iostream>
#include <string>

extern "C"
{
#include <evercrypt/EverCrypt_AutoConfig2.h>
}

using namespace ccfapp;
using namespace ccf;
using namespace std;
using namespace jsonrpc;
using namespace nlohmann;

// used throughout
auto kp = tls::make_key_pair();
auto member_cert = kp -> self_sign("CN=name_member");
auto verifier_mem = tls::make_verifier(member_cert);
auto member_caller = verifier_mem -> der_cert_data();
auto user_cert = kp -> self_sign("CN=name_user");
std::vector<uint8_t> dummy_key_share = {1, 2, 3};

auto encryptor = std::make_shared<ccf::NullTxEncryptor>();

constexpr auto default_pack = jsonrpc::Pack::Text;

string get_script_path(string name)
{
  auto default_dir = "../src/runtime_config";
  auto dir = getenv("RUNTIME_CONFIG_DIR");
  stringstream ss;
  ss << (dir ? dir : default_dir) << "/" << name;
  return ss.str();
}
const auto gov_script_file = files::slurp_string(get_script_path("gov.lua"));
const auto gov_veto_script_file =
  files::slurp_string(get_script_path("gov_veto.lua"));
const auto operator_gov_script_file =
  files::slurp_string(get_script_path("operator_gov.lua"));

template <typename E>
void check_error(const nlohmann::json& j, const E expected)
{
  CHECK(
    j[ERR][CODE].get<jsonrpc::ErrorBaseType>() ==
    static_cast<jsonrpc::ErrorBaseType>(expected));
}

void check_success(const Response<bool> r, const bool expected = true)
{
  CHECK(r.result == expected);
}

void set_whitelists(GenesisGenerator& gen)
{
  for (const auto& wl : default_whitelists)
    gen.set_whitelist(wl.first, wl.second);
}

std::vector<uint8_t> sign_json(nlohmann::json j, tls::KeyPairPtr& kp_)
{
  auto contents = nlohmann::json::to_msgpack(j);
  return kp_->sign(contents);
}

std::vector<uint8_t> create_request(
  const json& params, const string& method_name)
{
  http::Request r(method_name);
  const auto body = params.is_null() ? std::vector<uint8_t>() :
                                       jsonrpc::pack(params, default_pack);
  r.set_body(&body);
  return r.build_request();
}

std::vector<uint8_t> create_signed_request(
  const json& params, const string& method_name, tls::KeyPairPtr& kp_)
{
  http::Request r(method_name);

  const auto body = params.is_null() ? std::vector<uint8_t>() :
                                       jsonrpc::pack(params, default_pack);

  r.set_body(&body);
  http::sign_request(r, kp_);

  return r.build_request();
}

template <typename T>
auto query_params(T script, bool compile)
{
  json params;
  if (compile)
    params["bytecode"] = lua::compile(script);
  else
    params["text"] = script;
  return params;
}

template <typename T>
auto read_params(const T& key, const string& table_name)
{
  json params;
  params["key"] = key;
  params["table"] = table_name;
  return params;
}

json frontend_process(
  MemberRpcFrontend& frontend,
  const std::vector<uint8_t>& serialized_request,
  const Cert& caller)
{
  const enclave::SessionContext session(
    0, tls::make_verifier(caller)->der_cert_data());
  auto rpc_ctx = enclave::make_rpc_context(session, serialized_request);
  auto serialized_response = frontend.process(rpc_ctx);

  CHECK(serialized_response.has_value());

  http::SimpleMsgProcessor processor;
  http::Parser parser(HTTP_RESPONSE, processor);

  const auto parsed_count =
    parser.execute(serialized_response->data(), serialized_response->size());
  REQUIRE(parsed_count == serialized_response->size());
  REQUIRE(processor.received.size() == 1);

  return jsonrpc::unpack(processor.received.front().body, default_pack);
}

nlohmann::json get_proposal(
  MemberRpcFrontend& frontend, size_t proposal_id, const Cert& caller)
{
  Script read_proposal(fmt::format(
    R"xxx(
      tables = ...
      return tables["ccf.proposals"]:get({})
    )xxx",
    proposal_id));

  const auto read = create_request(read_proposal, "query");

  return frontend_process(frontend, read, caller);
}

std::vector<uint8_t> get_cert_data(uint64_t member_id, tls::KeyPairPtr& kp_mem)
{
  return kp_mem->self_sign("CN=new member" + to_string(member_id));
}

auto init_frontend(
  NetworkTables& network,
  GenesisGenerator& gen,
  StubNodeState& node,
  const int n_members,
  std::vector<std::vector<uint8_t>>& member_certs)
{
  // create members
  for (uint8_t i = 0; i < n_members; i++)
  {
    member_certs.push_back(get_cert_data(i, kp));
    gen.add_member(member_certs.back(), {}, MemberStatus::ACTIVE);
  }

  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();

  return MemberRpcFrontend(network, node);
}

TEST_CASE("Member query/read")
{
  // initialize the network state
  NetworkTables network;
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();
  const auto member_id =
    gen.add_member(member_cert, {}, MemberStatus::ACCEPTED);
  gen.finalize();

  const enclave::SessionContext member_session(
    enclave::InvalidSessionId, member_cert);

  // put value to read
  constexpr auto key = 123;
  constexpr auto value = 456;
  Store::Tx tx;
  tx.get_view(network.values)->put(key, value);
  CHECK(tx.commit() == kv::CommitSuccess::OK);

  static constexpr auto query = R"xxx(
  local tables = ...
  return tables["ccf.values"]:get(123)
  )xxx";

  SUBCASE("Query: bytecode/script allowed access")
  {
    // set member ACL so that the VALUES table is accessible
    Store::Tx tx;
    tx.get_view(network.whitelists)
      ->put(WlIds::MEMBER_CAN_READ, {Tables::VALUES});
    CHECK(tx.commit() == kv::CommitSuccess::OK);

    bool compile = true;
    do
    {
      const auto req = create_request(query_params(query, compile), "query");
      const Response<int> r = frontend_process(frontend, req, member_cert);
      CHECK(r.result == value);
      compile = !compile;
    } while (!compile);
  }

  SUBCASE("Query: table not in ACL")
  {
    // set member ACL so that no table is accessible
    Store::Tx tx;
    tx.get_view(network.whitelists)->put(WlIds::MEMBER_CAN_READ, {});
    CHECK(tx.commit() == kv::CommitSuccess::OK);

    auto req = create_request(query_params(query, true), "query");
    const auto response = frontend_process(frontend, req, member_cert);

    check_error(response, CCFErrorCodes::SCRIPT_ERROR);
  }

  SUBCASE("Read: allowed access, key exists")
  {
    Store::Tx tx;
    tx.get_view(network.whitelists)
      ->put(WlIds::MEMBER_CAN_READ, {Tables::VALUES});
    CHECK(tx.commit() == kv::CommitSuccess::OK);

    auto read_call =
      create_request(read_params<int>(key, Tables::VALUES), "read");
    const Response<int> r = frontend_process(frontend, read_call, member_cert);

    CHECK(r.result == value);
  }

  SUBCASE("Read: allowed access, key doesn't exist")
  {
    constexpr auto wrong_key = 321;
    Store::Tx tx;
    tx.get_view(network.whitelists)
      ->put(WlIds::MEMBER_CAN_READ, {Tables::VALUES});
    CHECK(tx.commit() == kv::CommitSuccess::OK);

    auto read_call =
      create_request(read_params<int>(wrong_key, Tables::VALUES), "read");
    const auto response = frontend_process(frontend, read_call, member_cert);

    check_error(response, StandardErrorCodes::INVALID_PARAMS);
  }

  SUBCASE("Read: access not allowed")
  {
    Store::Tx tx;
    tx.get_view(network.whitelists)->put(WlIds::MEMBER_CAN_READ, {});
    CHECK(tx.commit() == kv::CommitSuccess::OK);

    auto read_call =
      create_request(read_params<int>(key, Tables::VALUES), "read");
    const auto response = frontend_process(frontend, read_call, member_cert);

    check_error(response, CCFErrorCodes::SCRIPT_ERROR);
  }
}

TEST_CASE("Proposer ballot")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();

  const auto proposer_cert = get_cert_data(0, kp);
  const auto proposer_id =
    gen.add_member(proposer_cert, {}, MemberStatus::ACTIVE);
  const auto voter_cert = get_cert_data(1, kp);
  const auto voter_id = gen.add_member(voter_cert, {}, MemberStatus::ACTIVE);

  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();

  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  size_t proposal_id;

  const ccf::Script vote_for("return true");
  const ccf::Script vote_against("return false");
  {
    INFO("Propose, initially voting against");

    const auto proposed_member = get_cert_data(2, kp);

    Propose::In proposal;
    proposal.script = std::string(R"xxx(
      tables, member_info = ...
      return Calls:call("new_member", member_info)
    )xxx");
    proposal.parameter["cert"] = proposed_member;
    proposal.parameter["keyshare"] = dummy_key_share;
    proposal.ballot = vote_against;
    const auto propose = create_request(proposal, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, proposer_cert);

    // the proposal should be accepted, but not succeed immediately
    CHECK(r.result.completed == false);

    proposal_id = r.result.id;
  }

  {
    INFO("Second member votes for proposal");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r = frontend_process(frontend, vote, voter_cert);

    // The vote should not yet succeed
    CHECK(r.result == false);
  }

  {
    INFO("Read current votes");

    const auto read = create_signed_request(
      read_params(proposal_id, Tables::PROPOSALS), "read", kp);
    const Response<Proposal> proposal =
      get_proposal(frontend, proposal_id, proposer_cert);

    const auto& votes = proposal.result.votes;
    CHECK(votes.size() == 2);

    const auto proposer_vote = votes.find(proposer_id);
    CHECK(proposer_vote != votes.end());
    CHECK(proposer_vote->second == vote_against);

    const auto voter_vote = votes.find(voter_id);
    CHECK(voter_vote != votes.end());
    CHECK(voter_vote->second == vote_for);
  }

  {
    INFO("Proposer votes for");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r = frontend_process(frontend, vote, proposer_cert);

    // The vote should now succeed
    CHECK(r.result == true);
  }
}

struct NewMember
{
  MemberId id;
  tls::KeyPairPtr kp = tls::make_key_pair();
  Cert cert;
};

TEST_CASE("Add new members until there are 7 then reject")
{
  constexpr auto initial_members = 3;
  constexpr auto n_new_members = 7;
  constexpr auto max_members = 8;
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  // add three initial active members
  // the proposer
  auto proposer_id = gen.add_member(member_cert, {}, MemberStatus::ACTIVE);

  // the voters
  const auto voter_a_cert = get_cert_data(1, kp);
  auto voter_a = gen.add_member(voter_a_cert, {}, MemberStatus::ACTIVE);
  const auto voter_b_cert = get_cert_data(2, kp);
  auto voter_b = gen.add_member(voter_b_cert, {}, MemberStatus::ACTIVE);

  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  vector<NewMember> new_members(n_new_members);

  auto i = 0ul;
  for (auto& new_member : new_members)
  {
    const auto proposal_id = i;
    new_member.id = initial_members + i++;

    // new member certificate
    auto cert_pem =
      new_member.kp->self_sign(fmt::format("CN=new member{}", new_member.id));
    auto keyshare = dummy_key_share;
    auto v = tls::make_verifier(cert_pem);
    const auto _cert = v->raw();
    new_member.cert = {_cert->raw.p, _cert->raw.p + _cert->raw.len};

    // check new_member id does not work before member is added
    const auto read_next_req = create_request(
      read_params<int>(ValueIds::NEXT_MEMBER_ID, Tables::VALUES), "read");
    const auto r = frontend_process(frontend, read_next_req, new_member.cert);
    check_error(r, CCFErrorCodes::INVALID_CALLER_ID);

    // propose new member, as proposer
    Propose::In proposal;
    proposal.script = std::string(R"xxx(
      tables, member_info = ...
      return Calls:call("new_member", member_info)
    )xxx");
    proposal.parameter["cert"] = cert_pem;
    proposal.parameter["keyshare"] = keyshare;

    const auto propose = create_request(proposal, "propose");

    {
      Response<Propose::Out> r =
        frontend_process(frontend, propose, member_cert);

      // the proposal should be accepted, but not succeed immediately
      CHECK(r.result.id == proposal_id);
      CHECK(r.result.completed == false);
    }

    // read initial proposal, as second member
    const Response<Proposal> initial_read =
      get_proposal(frontend, proposal_id, voter_a_cert);
    CHECK(initial_read.result.proposer == proposer_id);
    CHECK(initial_read.result.script == proposal.script);
    CHECK(initial_read.result.parameter == proposal.parameter);

    // vote as second member
    Script vote_ballot(fmt::format(
      R"xxx(
        local tables, calls = ...
        local n = 0
        tables["ccf.members"]:foreach( function(k, v) n = n + 1 end )
        if n < {} then
          return true
        else
          return false
        end
      )xxx",
      max_members));

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_ballot}, "vote", kp);

    {
      Response<bool> r = frontend_process(frontend, vote, voter_a_cert);

      if (new_member.id < max_members)
      {
        // vote should succeed
        CHECK(r.result);
        // check that member with the new new_member cert can make RPCs now
        CHECK(
          Response<int>(
            frontend_process(frontend, read_next_req, new_member.cert))
            .result == new_member.id + 1);

        // successful proposals are removed from the kv, so we can't confirm
        // their final state
      }
      else
      {
        // vote should not succeed
        CHECK(!r.result);
        // check that member with the new new_member cert can make RPCs now
        check_error(
          frontend_process(frontend, read_next_req, new_member.cert),
          CCFErrorCodes::INVALID_CALLER_ID);

        // re-read proposal, as second member
        const Response<Proposal> final_read =
          get_proposal(frontend, proposal_id, voter_a_cert);
        CHECK(final_read.result.proposer == proposer_id);
        CHECK(final_read.result.script == proposal.script);
        CHECK(final_read.result.parameter == proposal.parameter);

        const auto my_vote = final_read.result.votes.find(voter_a);
        CHECK(my_vote != final_read.result.votes.end());
        CHECK(my_vote->second == vote_ballot);
      }
    }
  }

  SUBCASE("ACK from newly added members")
  {
    // iterate over all new_members, except for the last one
    for (auto new_member = new_members.cbegin(); new_member !=
         new_members.cend() - (initial_members + n_new_members - max_members);
         new_member++)
    {
      // (1) read ack entry
      const auto read_nonce_req = create_request(
        read_params(new_member->id, Tables::MEMBER_ACKS), "read");
      const Response<MemberAck> ack0 =
        frontend_process(frontend, read_nonce_req, new_member->cert);

      // (2) ask for a fresher nonce
      const auto freshen_nonce_req = create_request(nullptr, "updateAckNonce");
      check_success(
        frontend_process(frontend, freshen_nonce_req, new_member->cert));

      // (3) read ack entry again and check that the nonce has changed
      const Response<MemberAck> ack1 =
        frontend_process(frontend, read_nonce_req, new_member->cert);
      CHECK(ack0.result.next_nonce != ack1.result.next_nonce);

      // (4) sign old nonce and send it
      const auto bad_sig =
        RawSignature{new_member->kp->sign(ack0.result.next_nonce)};
      const auto send_bad_sig_req = create_request(bad_sig, "ack");
      check_error(
        frontend_process(frontend, send_bad_sig_req, new_member->cert),
        jsonrpc::StandardErrorCodes::INVALID_PARAMS);

      // (5) sign new nonce and send it
      const auto good_sig =
        RawSignature{new_member->kp->sign(ack1.result.next_nonce)};
      const auto send_good_sig_req = create_request(good_sig, "ack");
      check_success(
        frontend_process(frontend, send_good_sig_req, new_member->cert));

      // (6) read ack entry again and check that the signature matches
      const Response<MemberAck> ack2 =
        frontend_process(frontend, read_nonce_req, new_member->cert);
      CHECK(ack2.result.sig == good_sig.sig);

      // (7) read own member status
      const auto read_status_req =
        create_request(read_params(new_member->id, Tables::MEMBERS), "read");
      const Response<MemberInfo> mi =
        frontend_process(frontend, read_status_req, new_member->cert);
      CHECK(mi.result.status == MemberStatus::ACTIVE);
    }
  }
}

TEST_CASE("Accept node")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  auto new_kp = tls::make_key_pair();

  const auto member_0_cert = get_cert_data(0, new_kp);
  const auto member_1_cert = get_cert_data(1, kp);
  const auto member_0 = gen.add_member(member_0_cert, {}, MemberStatus::ACTIVE);
  const auto member_1 = gen.add_member(member_1_cert, {}, MemberStatus::ACTIVE);

  // node to be tested
  // new node certificate
  auto new_ca = new_kp->self_sign("CN=new node");
  NodeInfo ni;
  ni.cert = new_ca;
  gen.add_node(ni);
  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();
  MemberRpcFrontend frontend(network, node);
  frontend.open();
  auto node_id = 0;

  // check node exists with status pending
  {
    auto read_values =
      create_request(read_params<int>(node_id, Tables::NODES), "read");
    Response<NodeInfo> r =
      frontend_process(frontend, read_values, member_0_cert);

    CHECK(r.result.status == NodeStatus::PENDING);
  }

  // m0 proposes adding new node
  {
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("trust_node", node_id)
    )xxx");
    const auto propose =
      create_request(Propose::In{proposal, node_id}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_0_cert);

    CHECK(!r.result.completed);
    CHECK(r.result.id == 0);
  }

  // m1 votes for accepting a single new node
  {
    Script vote_ballot(R"xxx(
        local tables, calls = ...
        return #calls == 1 and calls[1].func == "trust_node"
       )xxx");
    const auto vote = create_signed_request(Vote{0, vote_ballot}, "vote", kp);

    check_success(frontend_process(frontend, vote, member_1_cert));
  }

  // check node exists with status pending
  {
    const auto read_values =
      create_request(read_params<int>(node_id, Tables::NODES), "read");
    Response<NodeInfo> r =
      frontend_process(frontend, read_values, member_0_cert);
    CHECK(r.result.status == NodeStatus::TRUSTED);
  }

  // m0 proposes retire node
  {
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("retire_node", node_id)
    )xxx");
    const auto propose =
      create_request(Propose::In{proposal, node_id}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_0_cert);

    CHECK(!r.result.completed);
    CHECK(r.result.id == 1);
  }

  // m1 votes for retiring node
  {
    const Script vote_ballot("return true");
    const auto vote = create_signed_request(Vote{1, vote_ballot}, "vote", kp);
    check_success(frontend_process(frontend, vote, member_1_cert));
  }

  // check that node exists with status retired
  {
    auto read_values =
      create_request(read_params<int>(node_id, Tables::NODES), "read");
    Response<NodeInfo> r =
      frontend_process(frontend, read_values, member_0_cert);
    CHECK(r.result.status == NodeStatus::RETIRED);
  }

  // check that retired node cannot be trusted
  {
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("trust_node", node_id)
    )xxx");
    const auto propose =
      create_request(Propose::In{proposal, node_id}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_0_cert);

    const Script vote_ballot("return true");
    const auto vote = create_signed_request(Vote{2, vote_ballot}, "vote", kp);
    check_error(
      frontend_process(frontend, vote, member_1_cert),
      StandardErrorCodes::INTERNAL_ERROR);
  }

  // check that retired node cannot be retired again
  {
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("retire_node", node_id)
    )xxx");
    const auto propose =
      create_request(Propose::In{proposal, node_id}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_0_cert);

    const Script vote_ballot("return true");
    const auto vote = create_signed_request(Vote{3, vote_ballot}, "vote", kp);
    check_error(
      frontend_process(frontend, vote, member_1_cert),
      StandardErrorCodes::INTERNAL_ERROR);
  }
}

bool test_raw_writes(
  NetworkTables& network,
  GenesisGenerator& gen,
  StubNodeState& node,
  Propose::In proposal,
  const int n_members = 1,
  const int pro_votes = 1,
  bool explicit_proposer_vote = false)
{
  std::vector<std::vector<uint8_t>> member_certs;
  auto frontend = init_frontend(network, gen, node, n_members, member_certs);
  frontend.open();

  // check values before
  {
    Store::Tx tx;
    auto next_member_id_r =
      tx.get_view(network.values)->get(ValueIds::NEXT_MEMBER_ID);
    CHECK(next_member_id_r);
    CHECK(*next_member_id_r == n_members);
  }

  // propose
  const auto proposal_id = 0ul;
  {
    const uint8_t proposer_id = 0;
    const auto propose = create_request(proposal, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_certs[0]);

    CHECK(r.result.completed == (n_members == 1));
    CHECK(r.result.id == proposal_id);
    if (r.result.completed)
      return true;
  }

  // con votes
  for (int i = n_members - 1; i >= pro_votes; i--)
  {
    const Script vote("return false");
    const auto vote_serialized =
      create_signed_request(Vote{proposal_id, vote}, "vote", kp);

    check_success(
      frontend_process(frontend, vote_serialized, member_certs[i]), false);
  }

  // pro votes (proposer also votes)
  bool completed = false;
  for (uint8_t i = explicit_proposer_vote ? 0 : 1; i < pro_votes; i++)
  {
    const Script vote("return true");
    const auto vote_serialized =
      create_signed_request(Vote{proposal_id, vote}, "vote", kp);
    if (!completed)
    {
      completed = Response<bool>(frontend_process(
                                   frontend, vote_serialized, member_certs[i]))
                    .result;
    }
    else
    {
      // proposal has been accepted - additional votes return an error
      check_error(
        frontend_process(frontend, vote_serialized, member_certs[i]),
        StandardErrorCodes::INVALID_PARAMS);
    }
  }
  return completed;
}

TEST_CASE("Propose raw writes")
{
  SUBCASE("insensitive tables")
  {
    const auto n_members = 10;
    for (int pro_votes = 0; pro_votes <= n_members; pro_votes++)
    {
      const bool should_succeed = pro_votes > n_members / 2;
      NetworkTables network;
      network.tables->set_encryptor(encryptor);
      Store::Tx gen_tx;
      GenesisGenerator gen(network, gen_tx);
      gen.init_values();
      StubNodeState node;
      // manually add a member in state active (not recommended)
      const Cert member_cert = {1, 2, 3};
      nlohmann::json params;
      params["cert"] = member_cert;
      params["keyshare"] = dummy_key_share;
      CHECK(
        test_raw_writes(
          network,
          gen,
          node,
          {R"xxx(
        local tables, param = ...
        local STATE_ACTIVE = "ACTIVE"
        local NEXT_MEMBER_ID_VALUE = 0
        local p = Puts:new()
        -- get id
        local member_id = tables["ccf.values"]:get(NEXT_MEMBER_ID_VALUE)
        -- increment id
        p:put("ccf.values", NEXT_MEMBER_ID_VALUE, member_id + 1)
        -- write member info and status
        p:put("ccf.members", member_id, {cert = param.cert, keyshare = param.keyshare, status = STATE_ACTIVE})
        p:put("ccf.member_certs", param.cert, member_id)
        return Calls:call("raw_puts", p)
      )xxx"s,
           params},
          n_members,
          pro_votes) == should_succeed);
      if (!should_succeed)
        continue;

      // check results
      Store::Tx tx;
      const auto next_mid =
        tx.get_view(network.values)->get(ValueIds::NEXT_MEMBER_ID);
      CHECK(next_mid);
      CHECK(*next_mid == n_members + 1);
      const auto m = tx.get_view(network.members)->get(n_members);
      CHECK(m);
      CHECK(m->status == MemberStatus::ACTIVE);
      const auto member_id =
        tx.get_view(network.member_certs)->get(member_cert);
      CHECK(member_id);
      CHECK(*member_id == n_members);
    }
  }

  SUBCASE("sensitive tables")
  {
    // propose changes to sensitive tables; changes must only be accepted
    // unanimously create new network for each case
    const auto sensitive_tables = {Tables::WHITELISTS, Tables::GOV_SCRIPTS};
    const auto n_members = 10;
    // let proposer vote/not vote
    for (const auto proposer_vote : {true, false})
    {
      for (int pro_votes = 0; pro_votes < n_members; pro_votes++)
      {
        for (const auto& sensitive_table : sensitive_tables)
        {
          NetworkTables network;
          network.tables->set_encryptor(encryptor);
          Store::Tx gen_tx;
          GenesisGenerator gen(network, gen_tx);
          gen.init_values();
          StubNodeState node;

          const auto sensitive_put =
            "return Calls:call('raw_puts', Puts:put('"s + sensitive_table +
            "', 9, {'aaa'}))"s;
          CHECK(
            test_raw_writes(
              network,
              gen,
              node,
              {sensitive_put},
              n_members,
              pro_votes,
              proposer_vote) == (n_members == pro_votes));
        }
      }
    }
  }
}

TEST_CASE("Remove proposal")
{
  NewMember caller;
  auto cert = caller.kp->self_sign("CN=new member");
  auto v = tls::make_verifier(cert);
  caller.cert = v->der_cert_data();

  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();

  StubNodeState node;
  gen.add_member(member_cert, {}, MemberStatus::ACTIVE);
  gen.add_member(cert, {}, MemberStatus::ACTIVE);
  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();
  MemberRpcFrontend frontend(network, node);
  frontend.open();
  auto proposal_id = 0;
  auto wrong_proposal_id = 1;
  ccf::Script proposal_script(R"xxx(
      local tables, param = ...
      return {}
    )xxx");

  // check that the proposal doesn't exist
  {
    Store::Tx tx;
    auto proposal = tx.get_view(network.proposals)->get(proposal_id);
    CHECK(!proposal);
  }

  {
    const auto propose =
      create_request(Propose::In{proposal_script, 0}, "propose");
    Response<Propose::Out> r = frontend_process(frontend, propose, member_cert);

    CHECK(r.result.id == proposal_id);
    CHECK(!r.result.completed);
  }

  // check that the proposal is there
  {
    Store::Tx tx;
    auto proposal = tx.get_view(network.proposals)->get(proposal_id);
    CHECK(proposal);
    CHECK(proposal->state == ProposalState::OPEN);
    CHECK(proposal->script.text.value() == proposal_script.text.value());
  }

  SUBCASE("Attempt withdraw proposal with non existing id")
  {
    json param;
    param["id"] = wrong_proposal_id;
    const auto withdraw = create_request(param, "withdraw");

    check_error(
      frontend_process(frontend, withdraw, member_cert),
      StandardErrorCodes::INVALID_PARAMS);
  }

  SUBCASE("Attempt withdraw proposal that you didn't propose")
  {
    json param;
    param["id"] = proposal_id;
    const auto withdraw = create_request(param, "withdraw");

    check_error(
      frontend_process(frontend, withdraw, cert),
      CCFErrorCodes::INVALID_CALLER_ID);
  }

  SUBCASE("Successfully withdraw proposal")
  {
    json param;
    param["id"] = proposal_id;
    const auto withdraw = create_request(param, "withdraw");

    check_success(frontend_process(frontend, withdraw, member_cert));

    // check that the proposal is now withdrawn
    {
      Store::Tx tx;
      auto proposal = tx.get_view(network.proposals)->get(proposal_id);
      CHECK(proposal.has_value());
      CHECK(proposal->state == ProposalState::WITHDRAWN);
    }
  }
}

TEST_CASE("Complete proposal after initial rejection")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  std::vector<std::vector<uint8_t>> member_certs;
  auto frontend = init_frontend(network, gen, node, 3, member_certs);
  frontend.open();

  {
    INFO("Propose");
    const auto proposal =
      "return Calls:call('raw_puts', Puts:put('ccf.values', 999, 999))"s;
    const auto propose = create_request(Propose::In{proposal}, "propose");

    Store::Tx tx;
    Response<Propose::Out> r =
      frontend_process(frontend, propose, member_certs[0]);
    CHECK(r.result.completed == false);
  }

  {
    INFO("Vote that rejects initially");
    const Script vote(R"xxx(
    local tables = ...
    return tables["ccf.values"]:get(123) == 123
    )xxx");
    const auto vote_serialized =
      create_signed_request(Vote{0, vote}, "vote", kp);

    check_success(
      frontend_process(frontend, vote_serialized, member_certs[1]), false);
  }

  {
    INFO("Try to complete");
    const auto complete = create_request(ProposalAction{0}, "complete");

    check_success(frontend_process(frontend, complete, member_certs[1]), false);
  }

  {
    INFO("Put value that makes vote agree");
    Store::Tx tx;
    tx.get_view(network.values)->put(123, 123);
    CHECK(tx.commit() == kv::CommitSuccess::OK);
  }

  {
    INFO("Try again to complete");
    const auto complete = create_request(ProposalAction{0}, "complete");

    check_success(frontend_process(frontend, complete, member_certs[1]));
  }
}

TEST_CASE("Vetoed proposal gets rejected")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  const auto voter_a_cert = get_cert_data(1, kp);
  auto voter_a = gen.add_member(voter_a_cert, {}, MemberStatus::ACTIVE);
  const auto voter_b_cert = get_cert_data(2, kp);
  auto voter_b = gen.add_member(voter_b_cert, {}, MemberStatus::ACTIVE);
  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_veto_script_file));
  gen.finalize();
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  Script proposal(R"xxx(
    tables, user_cert = ...
      return Calls:call("new_user", user_cert)
    )xxx");

  const vector<uint8_t> user_cert = kp->self_sign("CN=new user");
  const auto propose =
    create_request(Propose::In{proposal, user_cert}, "propose");

  Response<Propose::Out> r = frontend_process(frontend, propose, voter_a_cert);
  CHECK(r.result.completed == false);
  CHECK(r.result.id == 0);

  const ccf::Script vote_against("return false");
  {
    INFO("Member vetoes proposal");

    const auto vote = create_signed_request(Vote{0, vote_against}, "vote", kp);
    Response<bool> r = frontend_process(frontend, vote, voter_b_cert);

    CHECK(r.result == false);
  }

  {
    INFO("Check proposal was rejected");

    const Response<Proposal> proposal = get_proposal(frontend, 0, voter_a_cert);

    CHECK(proposal.result.state == ProposalState::REJECTED);
  }
}

TEST_CASE("Add user via proposed call")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  StubNodeState node;
  const auto member_cert = get_cert_data(0, kp);
  gen.add_member(member_cert, {}, MemberStatus::ACTIVE);
  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  Script proposal(R"xxx(
    tables, user_cert = ...
      return Calls:call("new_user", user_cert)
    )xxx");

  const vector<uint8_t> user_cert = kp->self_sign("CN=new user");
  const auto propose =
    create_request(Propose::In{proposal, user_cert}, "propose");

  Response<Propose::Out> r = frontend_process(frontend, propose, member_cert);
  CHECK(r.result.completed);
  CHECK(r.result.id == 0);

  Store::Tx tx1;
  const auto uid = tx1.get_view(network.values)->get(ValueIds::NEXT_USER_ID);
  CHECK(uid);
  CHECK(*uid == 1);
  const auto uid1 = tx1.get_view(network.user_certs)
                      ->get(tls::make_verifier(user_cert)->der_cert_data());
  CHECK(uid1);
  CHECK(*uid1 == 0);
}

TEST_CASE("Passing members ballot with operator")
{
  // Members pass a ballot with a constitution that includes an operator
  // Operator votes, but is _not_ taken into consideration
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();

  // Operating member, as set in operator_gov.lua
  const auto operator_cert = get_cert_data(0, kp);
  const auto operator_id =
    gen.add_member(operator_cert, {}, MemberStatus::ACTIVE);

  // Non-operating members
  std::map<size_t, ccf::Cert> members;
  for (size_t i = 1; i < 4; i++)
  {
    auto cert = get_cert_data(i, kp);
    members[gen.add_member(cert, {}, MemberStatus::ACTIVE)] = cert;
  }

  set_whitelists(gen);
  gen.set_gov_scripts(
    lua::Interpreter().invoke<json>(operator_gov_script_file));
  gen.finalize();

  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  size_t proposal_id;
  size_t proposer_id = 1;
  size_t voter_id = 2;

  const ccf::Script vote_for("return true");
  const ccf::Script vote_against("return false");
  {
    INFO("Propose and vote for");

    const auto proposed_member = get_cert_data(4, kp);

    Propose::In proposal;
    proposal.script = std::string(R"xxx(
      tables, member_info = ...
      return Calls:call("new_member", member_info)
    )xxx");
    proposal.parameter["cert"] = proposed_member;
    proposal.parameter["keyshare"] = dummy_key_share;
    proposal.ballot = vote_for;

    const auto propose = create_request(proposal, "propose");
    Response<Propose::Out> r = frontend_process(
      frontend,
      propose,
      tls::make_verifier(members[proposer_id])->der_cert_data());

    CHECK(r.result.completed == false);

    proposal_id = r.result.id;
  }

  {
    INFO("Operator votes, but without effect");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r = frontend_process(frontend, vote, operator_cert);

    CHECK(r.result == false);
  }

  {
    INFO("Second member votes for proposal, which passes");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r = frontend_process(frontend, vote, members[voter_id]);

    CHECK(r.result == true);
  }

  {
    INFO("Validate vote tally");

    const auto readj = create_signed_request(
      read_params(proposal_id, Tables::PROPOSALS), "read", kp);

    const Response<Proposal> proposal =
      get_proposal(frontend, proposal_id, members[proposer_id]);

    const auto& votes = proposal.result.votes;
    CHECK(votes.size() == 3);

    const auto operator_vote = votes.find(operator_id);
    CHECK(operator_vote != votes.end());
    CHECK(operator_vote->second == vote_for);

    const auto proposer_vote = votes.find(proposer_id);
    CHECK(proposer_vote != votes.end());
    CHECK(proposer_vote->second == vote_for);

    const auto voter_vote = votes.find(voter_id);
    CHECK(voter_vote != votes.end());
    CHECK(voter_vote->second == vote_for);
  }
}

TEST_CASE("Passing operator vote")
{
  // Operator issues a proposal that only requires its own vote
  // and gets it through without member votes
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  auto new_kp = tls::make_key_pair();
  auto new_ca = new_kp->self_sign("CN=new node");
  NodeInfo ni;
  ni.cert = new_ca;
  gen.add_node(ni);

  // Operating member, as set in operator_gov.lua
  const auto operator_cert = get_cert_data(0, kp);
  const auto operator_id =
    gen.add_member(operator_cert, {}, MemberStatus::ACTIVE);

  // Non-operating members
  std::map<size_t, ccf::Cert> members;
  for (size_t i = 1; i < 4; i++)
  {
    auto cert = get_cert_data(i, kp);
    members[gen.add_member(cert, {}, MemberStatus::ACTIVE)] = cert;
  }

  set_whitelists(gen);
  gen.set_gov_scripts(
    lua::Interpreter().invoke<json>(operator_gov_script_file));
  gen.finalize();

  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  size_t proposal_id;

  const ccf::Script vote_for("return true");
  const ccf::Script vote_against("return false");

  auto node_id = 0;
  {
    INFO("Check node exists with status pending");
    auto read_values =
      create_request(read_params<int>(node_id, Tables::NODES), "read");
    Response<NodeInfo> r =
      frontend_process(frontend, read_values, operator_cert);

    CHECK(r.result.status == NodeStatus::PENDING);
  }

  {
    INFO("Operator proposes and votes for node");
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("trust_node", node_id)
    )xxx");

    const auto propose =
      create_request(Propose::In{proposal, node_id, vote_for}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, operator_cert);

    CHECK(r.result.completed);
    proposal_id = r.result.id;
  }

  {
    INFO("Validate vote tally");

    const auto readj = create_signed_request(
      read_params(proposal_id, Tables::PROPOSALS), "read", kp);

    const Response<Proposal> proposal =
      get_proposal(frontend, proposal_id, operator_cert);

    const auto& votes = proposal.result.votes;
    CHECK(votes.size() == 1);

    const auto proposer_vote = votes.find(operator_id);
    CHECK(proposer_vote != votes.end());
    CHECK(proposer_vote->second == vote_for);
  }
}

TEST_CASE("Members passing an operator vote")
{
  // Operator proposes a vote, but does not vote for it
  // A majority of members pass the vote
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  auto new_kp = tls::make_key_pair();
  auto new_ca = new_kp->self_sign("CN=new node");
  NodeInfo ni;
  ni.cert = new_ca;
  gen.add_node(ni);

  // Operating member, as set in operator_gov.lua
  const auto operator_cert = get_cert_data(0, kp);
  const auto operator_id =
    gen.add_member(operator_cert, {}, MemberStatus::ACTIVE);

  // Non-operating members
  std::map<size_t, ccf::Cert> members;
  for (size_t i = 1; i < 4; i++)
  {
    auto cert = get_cert_data(i, kp);
    members[gen.add_member(cert, {}, MemberStatus::ACTIVE)] = cert;
  }

  set_whitelists(gen);
  gen.set_gov_scripts(
    lua::Interpreter().invoke<json>(operator_gov_script_file));
  gen.finalize();

  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  size_t proposal_id;

  const ccf::Script vote_for("return true");
  const ccf::Script vote_against("return false");

  auto node_id = 0;
  {
    INFO("Check node exists with status pending");
    const auto read_values =
      create_request(read_params<int>(node_id, Tables::NODES), "read");
    Response<NodeInfo> r =
      frontend_process(frontend, read_values, operator_cert);
    CHECK(r.result.status == NodeStatus::PENDING);
  }

  {
    INFO("Operator proposes and votes against adding node");
    Script proposal(R"xxx(
      local tables, node_id = ...
      return Calls:call("trust_node", node_id)
    )xxx");

    const auto propose =
      create_request(Propose::In{proposal, node_id, vote_against}, "propose");
    Response<Propose::Out> r =
      frontend_process(frontend, propose, operator_cert);

    CHECK(!r.result.completed);
    proposal_id = r.result.id;
  }

  size_t first_voter_id = 1;
  size_t second_voter_id = 2;

  {
    INFO("First member votes for proposal");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r =
      frontend_process(frontend, vote, members[first_voter_id]);

    CHECK(r.result == false);
  }

  {
    INFO("Second member votes for proposal");

    const auto vote =
      create_signed_request(Vote{proposal_id, vote_for}, "vote", kp);
    Response<bool> r =
      frontend_process(frontend, vote, members[second_voter_id]);

    CHECK(r.result == true);
  }

  {
    INFO("Validate vote tally");

    const auto readj = create_signed_request(
      read_params(proposal_id, Tables::PROPOSALS), "read", kp);

    const Response<Proposal> proposal =
      get_proposal(frontend, proposal_id, operator_cert);

    const auto& votes = proposal.result.votes;
    CHECK(votes.size() == 3);

    const auto proposer_vote = votes.find(operator_id);
    CHECK(proposer_vote != votes.end());
    CHECK(proposer_vote->second == vote_against);

    const auto first_vote = votes.find(first_voter_id);
    CHECK(first_vote != votes.end());
    CHECK(first_vote->second == vote_for);

    const auto second_vote = votes.find(second_voter_id);
    CHECK(second_vote != votes.end());
    CHECK(second_vote->second == vote_for);
  }
}

TEST_CASE("User data")
{
  NetworkTables network;
  network.tables->set_encryptor(encryptor);
  Store::Tx gen_tx;
  GenesisGenerator gen(network, gen_tx);
  gen.init_values();
  const auto member_id = gen.add_member(member_cert, {}, MemberStatus::ACTIVE);
  const auto user_id = gen.add_user(user_cert);
  set_whitelists(gen);
  gen.set_gov_scripts(lua::Interpreter().invoke<json>(gov_script_file));
  gen.finalize();

  StubNodeState node;
  MemberRpcFrontend frontend(network, node);
  frontend.open();

  const auto read_user_info =
    create_request(read_params(user_id, Tables::USERS), "read");

  {
    INFO("user data is initially empty");
    Response<ccf::UserInfo> read_response =
      frontend_process(frontend, read_user_info, member_cert);
    CHECK(read_response.result.user_data.is_null());
  }

  {
    auto user_data_object = nlohmann::json::object();
    user_data_object["name"] = "bob";
    user_data_object["permissions"] = {"read", "delete"};

    INFO("user data can be set to an object");
    Propose::In proposal;
    proposal.script = fmt::format(
      R"xxx(
        proposed_user_data = {{
          name = "bob",
          permissions = {{"read", "delete"}}
        }}
        return Calls:call("set_user_data", {{user_id = {}, user_data = proposed_user_data}})
      )xxx",
      user_id);
    const auto proposal_serialized = create_request(proposal, "propose");
    Response<Propose::Out> propose_response =
      frontend_process(frontend, proposal_serialized, member_cert);
    CHECK(propose_response.result.completed);

    INFO("user data object can be read");
    Response<ccf::UserInfo> read_response =
      frontend_process(frontend, read_user_info, member_cert);
    CHECK(read_response.result.user_data == user_data_object);
  }

  {
    const auto user_data_string = "ADMINISTRATOR";

    INFO("user data can be overwritten");
    Propose::In proposal;
    proposal.script = std::string(R"xxx(
      local tables, param = ...
      return Calls:call("set_user_data", {user_id = param.id, user_data = param.data})
    )xxx");
    proposal.parameter["id"] = user_id;
    proposal.parameter["data"] = user_data_string;
    const auto proposal_serialized = create_request(proposal, "propose");
    Response<Propose::Out> propose_response =
      frontend_process(frontend, proposal_serialized, member_cert);
    CHECK(propose_response.result.completed);

    INFO("user data object can be read");
    Response<ccf::UserInfo> response =
      frontend_process(frontend, read_user_info, member_cert);
    CHECK(response.result.user_data == user_data_string);
  }
}

// We need an explicit main to initialize kremlib and EverCrypt
int main(int argc, char** argv)
{
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  ::EverCrypt_AutoConfig2_init();
  int res = context.run();
  if (context.shouldExit())
    return res;
  return res;
}