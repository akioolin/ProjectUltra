#include "helpers/temp_dir.hpp"
#include "io/wav_io.hpp"
#include "ota_channel_core/session_manager.hpp"
#include "ota_simulator.grpc.pb.h"
#include "ota_simulator_service/auth_allowlist.hpp"
#include "ota_simulator_service/ota_simulator_service.hpp"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace {

namespace otasim = projectultra::otasim::v1;

void addToken(grpc::ClientContext& context, const std::string& token) {
    context.AddMetadata("authorization", "Bearer " + token);
}

bool hasSession(const otasim::ListSessionsResponse& response, const std::string& session_id) {
    for (const auto& session : response.sessions()) {
        if (session.session_id() == session_id) {
            return true;
        }
    }
    return false;
}

double rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

}  // namespace

int main() {
    ultra::test::TempDir temp("grpc_service_smoke");
    assert(temp.valid());

    const auto token_path = temp.child("tokens.conf");
    {
        std::ofstream out(token_path);
        // Alice runs the smoke test end-to-end including admin RPCs
        // (CreateSession + SetChannel), so she carries the admin role.
        // Bob stays operator-only to mirror a normal joined station.
        out << "alice_token:ALPHA:Alpha station:admin\n";
        out << "bob_token:BRAVO:Bravo station\n";
    }

    ultra::ota_simulator_service::AuthAllowlist auth;
    std::string error;
    assert(auth.loadFromFile(token_path, &error));

    ultra::ota_simulator_service::OtaSimulatorServiceConfig config;
    config.udp_bind_host = "127.0.0.1";
    config.udp_bind_port = 0;
    config.capture_root = temp.child("captures");

    ultra::ota_simulator_service::OtaSimulatorService service(std::move(auth), config);
    assert(service.start(&error));

    grpc::ServerBuilder builder;
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    assert(server);

    auto channel = server->InProcessChannel(grpc::ChannelArguments());
    auto stub = otasim::OtaSimulatorControl::NewStub(channel);

    otasim::RegisterStationRequest register_request;
    otasim::StationLease lease;
    grpc::ClientContext context;
    addToken(context, "alice_token");
    auto status = stub->RegisterStation(&context, register_request, &lease);
    assert(status.ok());
    assert(lease.station_id() == "ALPHA");
    assert(lease.callsign() == "ALPHA");

    otasim::StationLease invalid_lease;
    grpc::ClientContext invalid_context;
    addToken(invalid_context, "invalid_token");
    status = stub->RegisterStation(&invalid_context, register_request, &invalid_lease);
    assert(!status.ok());
    assert(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);

    otasim::HealthResponse health;
    grpc::ClientContext health_context;
    addToken(health_context, "alice_token");
    status = stub->Health(&health_context, otasim::HealthRequest{}, &health);
    assert(status.ok());
    assert(health.ok());

    otasim::CreateSessionRequest create_request;
    create_request.set_session_id("field");
    create_request.set_display_name("Field test");
    create_request.set_channel_model("awgn");
    create_request.set_snr_db(15.0);
    create_request.set_seed(7);
    create_request.set_station_cap(4);
    otasim::SessionInfo created;
    grpc::ClientContext create_context;
    addToken(create_context, "alice_token");
    status = stub->CreateSession(&create_context, create_request, &created);
    assert(status.ok());
    assert(created.session_id() == "field");

    otasim::JoinSessionRequest join_request;
    join_request.set_session_id("field");
    join_request.set_station_id("ALPHA");
    otasim::JoinSessionResponse join_response;
    grpc::ClientContext join_context;
    addToken(join_context, "alice_token");
    status = stub->JoinSession(&join_context, join_request, &join_response);
    assert(status.ok());
    assert(join_response.session().session_id() == "field");

    otasim::ListSessionsResponse sessions;
    grpc::ClientContext list_context;
    addToken(list_context, "alice_token");
    status = stub->ListSessions(&list_context, otasim::ListSessionsRequest{}, &sessions);
    assert(status.ok());
    assert(hasSession(sessions, ultra::ota_channel_core::kLobbySessionId));
    assert(hasSession(sessions, "field"));

    otasim::LeaveSessionRequest leave_request;
    leave_request.set_session_id("field");
    leave_request.set_station_id("ALPHA");
    google::protobuf::Empty empty;
    grpc::ClientContext leave_context;
    addToken(leave_context, "alice_token");
    status = stub->LeaveSession(&leave_context, leave_request, &empty);
    assert(status.ok());

    otasim::SetChannelRequest set_channel;
    set_channel.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    set_channel.set_model("good");
    set_channel.set_snr_db(12.0);
    set_channel.set_seed(99);
    otasim::CommandAck ack;
    grpc::ClientContext set_channel_context;
    addToken(set_channel_context, "alice_token");
    status = stub->SetChannel(&set_channel_context, set_channel, &ack);
    assert(status.ok());
    assert(ack.accepted());

    otasim::SetChannelRequest passthrough_channel;
    passthrough_channel.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    passthrough_channel.set_model("passthrough");
    passthrough_channel.set_snr_db(80.0);
    passthrough_channel.set_seed(99);
    otasim::CommandAck passthrough_ack;
    grpc::ClientContext passthrough_context;
    addToken(passthrough_context, "alice_token");
    status = stub->SetChannel(&passthrough_context, passthrough_channel, &passthrough_ack);
    assert(status.ok());
    assert(passthrough_ack.accepted());

    otasim::JoinSessionRequest lobby_join;
    lobby_join.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    lobby_join.set_station_id("ALPHA");
    otasim::JoinSessionResponse lobby_join_response;
    grpc::ClientContext lobby_join_context;
    addToken(lobby_join_context, "alice_token");
    status = stub->JoinSession(&lobby_join_context, lobby_join, &lobby_join_response);
    assert(status.ok());
    assert(lobby_join_response.session().station_count() == 1);

    auto lobby = service.sessionManager().getSession(ultra::ota_channel_core::kLobbySessionId);
    assert(lobby);
    assert(lobby->stationCount() == 1);

    otasim::InjectEffectRequest tone_inject;
    tone_inject.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    tone_inject.set_effect_type("tone");
    tone_inject.set_params_json("kind=tone;hz=1500;gain=0.35");
    tone_inject.set_start_sample(9600);
    tone_inject.set_duration_samples(4800);
    otasim::CommandAck tone_ack;
    grpc::ClientContext tone_context;
    addToken(tone_context, "alice_token");
    status = stub->InjectEffect(&tone_context, tone_inject, &tone_ack);
    assert(status.ok());
    assert(tone_ack.accepted());
    assert(lobby->stationCount() == 1);
    assert(!lobby->hasStation(ultra::ota_channel_core::kInjectedAudioStationId));

    const auto tone_before = lobby->receiveForStation("ALPHA", 9120, 480);
    const auto tone_during = lobby->receiveForStation("ALPHA", 9600, 4800);
    const auto tone_after = lobby->receiveForStation("ALPHA", 14400, 480);
    assert(rms(tone_before) == 0.0);
    assert(rms(tone_during) > 0.20);
    assert(rms(tone_after) == 0.0);

    const auto wav_path = temp.child("inject.wav");
    std::vector<float> wav_source(960, 0.0f);
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (size_t i = 0; i < wav_source.size(); ++i) {
        wav_source[i] = 0.45f * std::sin(kTwoPi * 1000.0 *
                                         static_cast<double>(i) / 48000.0);
    }
    assert(ultra::tools::io::writeWavPCM16Mono(wav_path.string(), wav_source));

    otasim::InjectEffectRequest wav_inject;
    wav_inject.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    wav_inject.set_effect_type("wav");
    wav_inject.set_params_json("kind=wav;path=" + wav_path.string() + ";gain=0.5");
    wav_inject.set_start_sample(19200);
    wav_inject.set_duration_samples(480);
    otasim::CommandAck wav_ack;
    grpc::ClientContext wav_context;
    addToken(wav_context, "alice_token");
    status = stub->InjectEffect(&wav_context, wav_inject, &wav_ack);
    assert(status.ok());
    assert(wav_ack.accepted());
    const auto wav_during = lobby->receiveForStation("ALPHA", 19200, 480);
    const auto wav_after = lobby->receiveForStation("ALPHA", 19680, 480);
    assert(rms(wav_during) > 0.10);
    assert(rms(wav_after) == 0.0);

    otasim::GetChannelRequest injected_channel_request;
    injected_channel_request.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    otasim::ChannelState injected_channel;
    grpc::ClientContext injected_channel_context;
    addToken(injected_channel_context, "alice_token");
    status = stub->GetChannel(&injected_channel_context,
                              injected_channel_request,
                              &injected_channel);
    assert(status.ok());
    assert(injected_channel.active_effects_size() >= 2);

    otasim::SetChannelRequest real_hf_without_bed;
    real_hf_without_bed.set_session_id(ultra::ota_channel_core::kLobbySessionId);
    real_hf_without_bed.set_model("real_hf_loop");
    real_hf_without_bed.set_snr_db(12.0);
    otasim::CommandAck rejected_ack;
    grpc::ClientContext real_hf_context;
    addToken(real_hf_context, "alice_token");
    status = stub->SetChannel(&real_hf_context, real_hf_without_bed, &rejected_ack);
    assert(!status.ok());
    assert(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    assert(status.error_message().find("--noise-bed-wav") != std::string::npos);

    service.beginDraining();
    otasim::HealthResponse draining_health;
    grpc::ClientContext draining_context;
    addToken(draining_context, "alice_token");
    status = stub->Health(&draining_context, otasim::HealthRequest{}, &draining_health);
    assert(status.ok());
    assert(!draining_health.ok());

    server->Shutdown();
    server->Wait();
    service.shutdown();

    std::cout << "grpc service smoke covered auth, sessions, channel, health, shutdown\n";
    return 0;
}
