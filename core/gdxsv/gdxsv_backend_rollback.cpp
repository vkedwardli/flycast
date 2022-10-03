#include "gdxsv_backend_rollback.h"

#include <future>
#include <map>
#include <string>
#include <vector>

#include "emulator.h"
#include "gdx_rpc.h"
#include "gdxsv.h"
#include "gdxsv.pb.h"
#include "hw/maple/maple_if.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "libs.h"
#include "network/ggpo.h"
#include "network/net_platform.h"
#include "rend/gui.h"
#include "rend/gui_util.h"
#include "rend/transform_matrix.h"

namespace {
u8 DummyGameParam[640] = {0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x05, 0x00, 0x04,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x76, 0x83, 0x8c, 0x83, 0x43,
                          0x83, 0x84, 0x81, 0x5b, 0x82, 0x50, 0x00, 0x00, 0x00, 0x00, 0x07};
u8 DummyRuleData[] = {0x03, 0x02, 0x03, 0x00, 0x00, 0x01, 0x58, 0x02, 0x58, 0x02, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xff, 0x3f, 0x00, 0x00,
                      0xff, 0x01, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xff, 0x3f, 0x00};

const u16 ExInputNone = 0;
const u16 ExInputWaitStart = 1;
const u16 ExInputWaitLoadEnd = 2;

// maple input to mcs pad input
u16 convertInput(MapleInputState input) {
    u16 r = 0;
    if (~input.kcode & 0x0004) r |= 0x4000;      // A
    if (~input.kcode & 0x0002) r |= 0x2000;      // B
    if (~input.kcode & 0x0400) r |= 0x0002;      // X
    if (~input.kcode & 0x0200) r |= 0x0001;      // Y
    if (~input.kcode & 0x0010) r |= 0x0020;      // up
    if (~input.kcode & 0x0020) r |= 0x0010;      // down
    if (~input.kcode & 0x0080) r |= 0x0004;      // right
    if (~input.kcode & 0x0040) r |= 0x0008;      // left
    if (~input.kcode & 0x0008) r |= 0x0080;      // Start
    if (~input.kcode & 0x00020000) r |= 0x8000;  // LT
    if (~input.kcode & 0x00040000) r |= 0x1000;  // RT

    if (input.fullAxes[0] + 128 <= 128 - 0x20) r |= 0x0008;  // left
    if (input.fullAxes[0] + 128 >= 128 + 0x20) r |= 0x0004;  // right
    if (input.fullAxes[1] + 128 <= 128 - 0x20) r |= 0x0020;  // up
    if (input.fullAxes[1] + 128 >= 128 + 0x20) r |= 0x0010;  // down
    return r;
}

float scale() {
    const auto W = ImGui::GetIO().DisplaySize.x;
    const auto H = ImGui::GetIO().DisplaySize.y;
    float renderAR = getOutputFramebufferAspectRatio();
    float screenAR = W / H;
    float dx = 0;
    float dy = 0;
    if (renderAR > screenAR)
        dy = H * (1 - screenAR / renderAR) / 2;
    else
        dx = W * (1 - renderAR / screenAR) / 2;

    return std::min((W - dx * 2) / 640.f, (H - dy * 2) / 480.f);
}

ImVec2 fromCenter(float x, float y) {
    const auto S = scale();
    const auto CX = ImGui::GetIO().DisplaySize.x / 2.f;
    const auto CY = ImGui::GetIO().DisplaySize.y / 2.f;

    return ImVec2(CX + (x * S), CY + (y * S));
}

static void screenToNative(int& x, int& y, int width, int height) {
    float fx, fy;
    float scale = 480.f / height;
    fy = y * scale;
    scale /= config::ScreenStretching / 100.f;
    fx = (x - (width - 640.f / scale) / 2.f) * scale;
    x = (int)std::round(fx);
    y = (int)std::round(fy);
}

float scaled(float size) {
    const auto S = scale();
    return S * size;
}

ImColor fadeColor(ImColor color, int elapsed) {
    if (elapsed <= 1800)
        color.Value.w *= (elapsed - 1550) / 250.0;
    else if ( elapsed >= 6600 && elapsed < 6900)
        color.Value.w *= 1.0 - (elapsed - 6600) / 300.0;
    
    return color;
}

ImColor msColor(int ms) {
    if (ms <= 30) return ImColor(63, 166, 214);
    if (ms <= 60) return ImColor(0, 168, 0);
    if (ms <= 90) return ImColor(255, 207, 0);
    if (ms <= 120) return ImColor(240, 105, 0);
    if (ms <= 150) return ImColor(173, 31, 0);
    return ImColor(36, 36, 36);
}

ImColor barColor(int ms, int n) {
    if (ms <= 30) return msColor(30);
    if (ms <= 60 && n < 4) return msColor(60);
    if (ms <= 90 && n < 3) return msColor(90);
    if (ms <= 120 && n < 2) return msColor(120);
    if (ms <= 150 && n < 1) return msColor(150);
    return msColor(999);
}

}  // namespace

// Designed by zetaeddie
void GdxsvBackendRollback::DisplayOSD() {
    const auto ms = ping_pong_.ElapsedMs();
    if (1550 < ms && ms < 6900) {
        uint8_t matrix[4][4];
        ping_pong_.GetRttMatrix(matrix);

        const auto W = ImGui::GetIO().DisplaySize.x;
        const auto H = ImGui::GetIO().DisplaySize.y;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2.f, ImGui::GetIO().DisplaySize.y / 2.f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##gdxsvosd", NULL,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoInputs);

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        
        // Draw latency background
        draw_list->AddRectFilled(fromCenter(-45, -97), fromCenter(45.25, -51.875), fadeColor(ImColor(0, 0, 0), ms));
        draw_list->AddRectFilled(fromCenter(-45, 53.125), fromCenter(45.25, 98.25), fadeColor(ImColor(0, 0, 0), ms));
        
        // Peer circle
        ImVec2 points[] = {fromCenter(-35.25, -87.5), fromCenter(35.5, -87.5), fromCenter(-35.25, 88.75), fromCenter(35.5, 88.75)};
        
        // Latency bars, bars[peer][target][bar][quad_4points]
        ImVec2 bars[][4][5][4] = {
            {
                //P1~P1
                {},
                //P1~P2
                {
                    {fromCenter(-25.75, -89.25), fromCenter(-23.25, -89.25), fromCenter(-23.25, -85.25), fromCenter(-25.75, -85.25)},
                    {fromCenter(-21.375, -90.238), fromCenter(-18.625, -90.238), fromCenter(-18.625, -84.238), fromCenter(-21.375, -84.238)},
                    {fromCenter(-16.75, -91.25), fromCenter(-13.75, -91.25), fromCenter(-13.75, -83.25), fromCenter(-16.75, -83.25)},
                    {fromCenter(-11.875, -92.75), fromCenter(-8.625, -92.75), fromCenter(-8.625, -81.75), fromCenter(-11.875, -81.75)},
                    {fromCenter(-6.75, -94.75), fromCenter(-3.25, -94.75), fromCenter(-3.25, -79.75), fromCenter(-6.75, -79.75)}
                },
                //P1~P3
                {
                    {fromCenter(-37.25, -78), fromCenter(-33.25, -78), fromCenter(-33.25, -75.5), fromCenter(-37.25, -75.5)},
                    {fromCenter(-38.262, -73.625), fromCenter(-32.262, -73.625), fromCenter(-32.262, -70.875), fromCenter(-38.262, -70.875)},
                    {fromCenter(-39.25, -69), fromCenter(-31.25, -69), fromCenter(-31.25, -66), fromCenter(-39.25, -66)},
                    {fromCenter(-40.75, -64.125), fromCenter(-29.75, -64.125), fromCenter(-29.75, -60.875), fromCenter(-40.75, -60.875)},
                    {fromCenter(-42.75, -59), fromCenter(-27.75, -59), fromCenter(-27.75, -55.5), fromCenter(-42.75, -55.5)}
                },
                //P1~P4
                {
                    {fromCenter(-28.654, -78.075), fromCenter(-25.825, -80.904), fromCenter(-24.058, -79.136), fromCenter(-26.886, -76.308)},
                    {fromCenter(-26.276, -74.266), fromCenter(-22.034, -78.508), fromCenter(-20.089, -76.564), fromCenter(-24.332, -72.321)},
                    {fromCenter(-23.704, -70.297), fromCenter(-18.047, -75.954), fromCenter(-15.926, -73.833), fromCenter(-21.583, -68.176)},
                    {fromCenter(-21.318, -65.789), fromCenter(-13.539, -73.568), fromCenter(-11.241, -71.269), fromCenter(-19.019, -63.491)},
                    {fromCenter(-19.108, -60.751), fromCenter(-8.501, -71.358), fromCenter(-6.026, -68.883), fromCenter(-16.633, -58.276)}
                }
            },
            {
                //P2~P1
                {
                    {fromCenter(23.5, -89.25), fromCenter(26.0, -89.25), fromCenter(26.0, -85.25), fromCenter(23.5, -85.25)},
                    {fromCenter(18.875, -90.238), fromCenter(21.625, -90.238), fromCenter(21.625, -84.238), fromCenter(18.875, -84.238)},
                    {fromCenter(14.0, -91.25), fromCenter(17.0, -91.25), fromCenter(17.0, -83.25), fromCenter(14.0, -83.25)},
                    {fromCenter(8.875, -92.75), fromCenter(12.125, -92.75), fromCenter(12.125, -81.75), fromCenter(8.875, -81.75)},
                    {fromCenter(3.5, -94.75), fromCenter(7.0, -94.75), fromCenter(7.0, -79.75), fromCenter(3.5, -79.75)}
                },
                //P2~P2
                {},
                //P2~P3
                {
                    {fromCenter(28.904, -78.075), fromCenter(26.075, -80.904), fromCenter(24.308, -79.136), fromCenter(27.136, -76.308)},
                    {fromCenter(26.526, -74.266), fromCenter(22.284, -78.508), fromCenter(20.339, -76.564), fromCenter(24.582, -72.321)},
                    {fromCenter(23.954, -70.297), fromCenter(18.297, -75.954), fromCenter(16.176, -73.833), fromCenter(21.833, -68.176)},
                    {fromCenter(21.568, -65.789), fromCenter(13.789, -73.568), fromCenter(11.491, -71.269), fromCenter(19.269, -63.491)},
                    {fromCenter(19.358, -60.751), fromCenter(8.751, -71.358), fromCenter(6.276, -68.883), fromCenter(16.883, -58.276)}
                },
                //P2~P4
                {
                    {fromCenter(33.5, -78), fromCenter(37.5, -78), fromCenter(37.5, -75.5), fromCenter(33.5, -75.5)},
                    {fromCenter(32.512, -73.625), fromCenter(38.512, -73.625), fromCenter(38.512, -70.875), fromCenter(32.512, -70.875)},
                    {fromCenter(31.5, -69), fromCenter(39.5, -69), fromCenter(39.5, -66), fromCenter(31.5, -66)},
                    {fromCenter(30.0, -64.125), fromCenter(41.0, -64.125), fromCenter(41.0, -60.875), fromCenter(30.0, -60.875)},
                    {fromCenter(28.0, -59), fromCenter(43.0, -59), fromCenter(43.0, -55.5), fromCenter(28.0, -55.5)}
                }
            },
            {
                //P3~P1
                {
                    {fromCenter(-37.25, 76.75), fromCenter(-33.25, 76.75), fromCenter(-33.25, 79.25), fromCenter(-37.25, 79.25)},
                    {fromCenter(-38.262, 72.125), fromCenter(-32.262, 72.125), fromCenter(-32.262, 74.875), fromCenter(-38.262, 74.875)},
                    {fromCenter(-39.25, 67.25), fromCenter(-31.25, 67.25), fromCenter(-31.25, 70.25), fromCenter(-39.25, 70.25)},
                    {fromCenter(-40.75, 62.125), fromCenter(-29.75, 62.125), fromCenter(-29.75, 65.375), fromCenter(-40.75, 65.375)},
                    {fromCenter(-42.75, 56.75), fromCenter(-27.75, 56.75), fromCenter(-27.75, 60.25), fromCenter(-42.75, 60.25)}
                },
                //P3~P2
                {
                    {fromCenter(-28.654, 79.325), fromCenter(-25.825, 82.154), fromCenter(-24.058, 80.386), fromCenter(-26.886, 77.558)},
                    {fromCenter(-26.276, 75.516), fromCenter(-22.034, 79.758), fromCenter(-20.089, 77.814), fromCenter(-24.332, 73.571)},
                    {fromCenter(-23.704, 71.547), fromCenter(-18.047, 77.204), fromCenter(-15.926, 75.083), fromCenter(-21.583, 69.426)},
                    {fromCenter(-21.318, 67.039), fromCenter(-13.539, 74.818), fromCenter(-11.241, 72.519), fromCenter(-19.019, 64.741)},
                    {fromCenter(-19.108, 62.001), fromCenter(-8.501, 72.608), fromCenter(-6.026, 70.133), fromCenter(-16.633, 59.526)}
                },
                //P3~P3
                {},
                //P3~P4
                {
                    {fromCenter(-25.75, 86.5), fromCenter(-23.25, 86.5), fromCenter(-23.25, 90.5), fromCenter(-25.75, 90.5)},
                    {fromCenter(-21.375, 85.488), fromCenter(-18.625, 85.488), fromCenter(-18.625, 91.488), fromCenter(-21.375, 91.488)},
                    {fromCenter(-16.75, 84.5), fromCenter(-13.75, 84.5), fromCenter(-13.75, 92.5), fromCenter(-16.75, 92.5)},
                    {fromCenter(-11.875, 83.0), fromCenter(-8.625, 83.0), fromCenter(-8.625, 94.0), fromCenter(-11.875, 94.0)},
                    {fromCenter(-6.75, 81.0), fromCenter(-3.25, 81.0), fromCenter(-3.25, 96.0), fromCenter(-6.75, 96.0)}
                }
            },
            {
                //P4~P1
                {
                    {fromCenter(28.904, 79.325), fromCenter(26.075, 82.154), fromCenter(24.308, 80.386), fromCenter(27.136, 77.558)},
                    {fromCenter(26.526, 75.516), fromCenter(22.284, 79.758), fromCenter(20.339, 77.814), fromCenter(24.582, 73.571)},
                    {fromCenter(23.954, 71.547), fromCenter(18.297, 77.204), fromCenter(16.176, 75.083), fromCenter(21.833, 69.426)},
                    {fromCenter(21.568, 67.039), fromCenter(13.789, 74.818), fromCenter(11.491, 72.519), fromCenter(19.269, 64.741)},
                    {fromCenter(19.358, 62.001), fromCenter(8.751, 72.608), fromCenter(6.276, 70.133), fromCenter(16.883, 59.526)}
                },
                //P4~P2
                {
                    {fromCenter(33.5, 76.75), fromCenter(37.5, 76.75), fromCenter(37.5, 79.25), fromCenter(33.5, 79.25)},
                    {fromCenter(32.512, 72.125), fromCenter(38.512, 72.125), fromCenter(38.512, 74.875), fromCenter(32.512, 74.875)},
                    {fromCenter(31.5, 67.25), fromCenter(39.5, 67.25), fromCenter(39.5, 70.25), fromCenter(31.5, 70.25)},
                    {fromCenter(30.0, 62.125), fromCenter(41.0, 62.125), fromCenter(41.0, 65.375), fromCenter(30.0, 65.375)},
                    {fromCenter(28.0, 56.75), fromCenter(43, 56.75), fromCenter(43.0, 60.25), fromCenter(28.0, 60.25)}
                },
                //P4~P3
                {
                    {fromCenter(23.5, 86.5), fromCenter(26.0, 86.5), fromCenter(26.0, 90.5), fromCenter(23.5, 90.5)},
                    {fromCenter(18.875, 85.488), fromCenter(21.625, 85.488), fromCenter(21.625, 91.488), fromCenter(18.875, 91.488)},
                    {fromCenter(14.0, 84.5), fromCenter(17.0, 84.5), fromCenter(17.0, 92.5), fromCenter(14.0, 92.5)},
                    {fromCenter(8.875, 83.0), fromCenter(12.125, 83.0), fromCenter(12.125, 94.0), fromCenter(8.875, 94.0)},
                    {fromCenter(3.5, 81.0), fromCenter(7.0, 81.0), fromCenter(7.0, 96.0), fromCenter(3.5, 96.0)}
                },
                //P4~P4
                {}
            }
        };
        
        // Draw peer circles
        for (int i = 0; i < 4; ++i) {
            //ignore 0 latency, choose lowest value to display
            std::vector<uint8_t> v(std::begin(matrix[i]), std::end(matrix[i]));
            std::sort(v.begin(), v.end(), std::greater<>());
            while(v.back() == 0 && v.size() > 1){
                v.pop_back();
            }
            draw_list->AddCircleFilled(points[i], scaled(4.5125), fadeColor(msColor(v.back()), ms), 20);
        }
        
        // Draw latency bars
        for (int peer = 0; peer < 4; ++peer) {
            for (int target = 0; target < 4; ++target) {
                if ( peer == target ) continue;
                for (int bar = 0; bar < 5; ++bar) {
                    draw_list->AddQuadFilled(
                        bars[peer][target][bar][0],
                        bars[peer][target][bar][1],
                        bars[peer][target][bar][2],
                        bars[peer][target][bar][3],
                        fadeColor(barColor(matrix[peer][target], bar), ms)
                    );
                }
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();
    }
}

void GdxsvBackendRollback::Reset() {
    RestorePatch();
    state_ = State::None;
    lbs_tx_reader_.Clear();
    recv_buf_.clear();
    recv_delay_ = 0;
}

void GdxsvBackendRollback::OnMainUiLoop() {
    const int COM_R_No0 = 0x0c391d79;  // TODO:disk2
    const int ConnectionStatus = 0x0c3abb84;  // TODO: disk2
    const int NetCountDown = 0x0c3ab942;  // TODO: disk2

    if (state_ == State::StopEmulator) {
        emu.stop();
        state_ = State::WaitPingPong;
    }

    if (state_ == State::WaitPingPong && !ping_pong_.Running()) {
        state_ = State::StartGGPOSession;
    }

    static auto session_start_time = std::chrono::high_resolution_clock::now();
    if (state_ == State::StartGGPOSession) {
        bool ok = true;
        std::vector<std::string> ips(matching_.player_count());
        std::vector<u16> ports(matching_.player_count());
        int max_rtt = 0;
        for (int i = 0; i < matching_.player_count(); i++) {
            if (i == matching_.peer_id()) {
                ips[i] = "";
                ports[i] = port_;
            } else {
                sockaddr_in addr;
                int rtt;
                if (ping_pong_.GetAvailableAddress(i, &addr, &rtt)) {
                    max_rtt = std::max(max_rtt, rtt);
                    char str[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &(addr.sin_addr), str, INET_ADDRSTRLEN);
                    ips[i] = str;
                    ports[i] = ntohs(addr.sin_port);
                } else {
                    NOTICE_LOG(COMMON, "No available address %d", i);
                    ok = false;
                }
            }
        }

        if (ok) {
            int delay = std::max(0, std::max(config::GdxMinDelay.get(), int(max_rtt / 2.0 / 16.66 + 0.5)));
            NOTICE_LOG(COMMON, "max_rtt=%d delay=%d", max_rtt, delay);
            config::GGPOEnable.override(1);
            config::GGPODelay.override(delay);
            start_network_ = ggpo::gdxsvStartNetwork(matching_.battle_code().c_str(), matching_.peer_id(), ips, ports);
            ggpo::receiveChatMessages(nullptr);
            session_start_time = std::chrono::high_resolution_clock::now();
            state_ = State::WaitGGPOSession;
        } else {
            gdxsv_WriteMem16(NetCountDown, 1);
            emu.start();
            state_ = State::End;
        }
    }

    if (state_ == State::WaitGGPOSession) {
        auto now = std::chrono::high_resolution_clock::now();
        auto timeout = 10000 <= std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start_time).count();

        if (start_network_.valid() && start_network_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            if (ggpo::active()) {
                start_network_ = std::future<bool>();
                state_ = State::McsInBattle;
                emu.start();
            }
            else {
                NOTICE_LOG(COMMON, "StartNetwork failure");
                state_ = State::End;
                emu.start();
            }
        }
        else if (timeout) {
            NOTICE_LOG(COMMON, "StartNetwork timeout");
            ggpo::stopSession();
            state_ = State::End;
            emu.start();
        }
    }

    // Rebattle end
    if (gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) == 3 && ggpo::active()) {
        ggpo::stopSession();
        state_ = State::End;
    }

    // Fast return to lobby on error
    if (gdxsv_ReadMem16(ConnectionStatus) == 1 &&
        gdxsv_ReadMem16(ConnectionStatus + 4) == 10 &&
        1 < gdxsv_ReadMem16(NetCountDown)) {
        ggpo::stopSession();
        state_ = State::End;
        gdxsv_WriteMem16(NetCountDown, 1);
    }
}

bool GdxsvBackendRollback::StartLocalTest(const char *param) {
    auto args = std::string(param);
    int me = 0;
    if (0 < args.size() && '1' <= args[0] && args[0] <= '4') {
        me = args[0] - '1';
    }
    Reset();
    DummyRuleData[6] = 1;
    DummyRuleData[7] = 0;
    DummyRuleData[8] = 1;
    DummyRuleData[9] = 0;
    state_ = State::StartLocalTest;
    maxlag_ = 0;
    matching_.set_battle_code("0123456");
    matching_.set_peer_id(me);
    matching_.set_session_id(12345);
    matching_.set_timeout_min_ms(1000);
    matching_.set_timeout_max_ms(10000);
    matching_.set_player_count(4);
    for (int i = 0; i < 4; i++) {
        proto::PlayerAddress player{};
        player.set_ip("127.0.0.1");
        player.set_port(10010 + i);
        player.set_user_id(std::to_string(i));
        player.set_peer_id(i);
        matching_.mutable_candidates()->Add(std::move(player));
    }
    Prepare(matching_, 10010 + me);
    NOTICE_LOG(COMMON, "RollbackNet StartLocalTest %d", me);
    return true;
}

void GdxsvBackendRollback::Prepare(const proto::P2PMatching &matching, int port) {
    matching_ = matching;
    port_ = port;

    ping_pong_.Reset();
    for (const auto &c : matching.candidates()) {
        if (c.peer_id() != matching_.peer_id()) {
            ping_pong_.AddCandidate(c.user_id(), c.peer_id(), c.ip(), c.port());
        }
    }
    ping_pong_.Start(matching.session_id(), matching.peer_id(), port, matching.timeout_min_ms(),
                     matching.timeout_max_ms());
}

void GdxsvBackendRollback::Open() {
    NOTICE_LOG(COMMON, "GdxsvBackendRollback.Open");
    recv_buf_.assign({0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd});
    state_ = State::McsSessionExchange;
    maxlag_ = 0;
    ApplyPatch(true);
}

void GdxsvBackendRollback::Close() {
    ggpo::stopSession();
    RestorePatch();
    state_ = State::End;
}

u32 GdxsvBackendRollback::OnSockWrite(u32 addr, u32 size) {
    if (state_ <= State::LbsStartBattleFlow) {
        u8 buf[InetBufSize];
        for (int i = 0; i < size; ++i) {
            buf[i] = gdxsv_ReadMem8(addr + i);
        }

        lbs_tx_reader_.Write((const char *)buf, size);
        ProcessLbsMessage();
    }

    ApplyPatch(false);
    return size;
}

u32 GdxsvBackendRollback::OnSockRead(u32 addr, u32 size) {
    if (state_ <= State::LbsStartBattleFlow) {
        ProcessLbsMessage();
    } else {
        int frame = 0;
        ggpo::getCurrentFrame(&frame);
        const int InetBuf = 0x0c3ab984;           // TODO: disk2
        const int ConnectionStatus = 0x0c3abb84;  // TODO: disk2
        // NOTICE_LOG(COMMON, "[FRAME:%4d :RBK=%d] State=%d OnSockRead CONNECTION: %d %d", frame, ggpo::rollbacking(), state_, gdxsv_ReadMem16(ConnectionStatus), gdxsv_ReadMem16(ConnectionStatus + 4));

        // Fast disconnect
        if (gdxsv_ReadMem16(ConnectionStatus + 4) < 10) {
            for (int i = 0; i < matching_.player_count(); ++i) {
                if (!ggpo::isConnected(i)) {
                    gdxsv_WriteMem16(ConnectionStatus + 4, 0x0a);
                    ggpo::setExInput(ExInputNone);
                    break;
                }
            }
        }

        int msg_len = gdxsv_ReadMem8(InetBuf);
        McsMessage msg;
        if (0 < msg_len) {
            if (msg_len == 0x82) {
                msg_len = 20;
            }
            msg.body.resize(msg_len);
            for (int i = 0; i < msg_len; i++) {
                msg.body[i] = gdxsv_ReadMem8(InetBuf + i);
                gdxsv_WriteMem8(InetBuf + i, 0);
            }

            switch (msg.Type()) {
                case McsMessage::MsgType::ConnectionIdMsg:
                    state_ = State::StopEmulator;
                    break;
                case McsMessage::MsgType::IntroMsg:
                    for (int i = 0; i < matching_.player_count(); i++) {
                        if (i != matching_.peer_id()) {
                            auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsg, i);
                            std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
                        }
                    }
                    break;
                case McsMessage::MsgType::IntroMsgReturn:
                    for (int i = 0; i < matching_.player_count(); i++) {
                        if (i != matching_.peer_id()) {
                            auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsgReturn, i);
                            std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
                        }
                    }
                    break;
                case McsMessage::MsgType::PingMsg:
                    for (int i = 0; i < matching_.player_count(); i++) {
                        if (i != matching_.peer_id()) {
                            auto pong_msg = McsMessage::Create(McsMessage::MsgType::PongMsg, i);
                            pong_msg.SetPongTo(matching_.peer_id());
                            pong_msg.PongCount(msg.PingCount());
                            std::copy(pong_msg.body.begin(), pong_msg.body.end(), std::back_inserter(recv_buf_));
                        }
                    }
                    break;
                case McsMessage::MsgType::PongMsg:
                    break;
                case McsMessage::MsgType::StartMsg:
                    if (!ggpo::rollbacking()) {
                        ggpo::setExInput(ExInputWaitStart);
                        NOTICE_LOG(COMMON, "StartMsg KeyFrame:%d", frame);
                    }
                    break;
                case McsMessage::MsgType::ForceMsg:
                    break;
                case McsMessage::MsgType::KeyMsg1:
                    for (int i = 0; i < matching_.player_count(); ++i) {
                        if (ggpo::isConnected(i)) {
                            auto msg = McsMessage::Create(McsMessage::KeyMsg1, i);
                            auto input = convertInput(mapleInputState[i]);
                            msg.body[2] = input >> 8 & 0xff;
                            msg.body[3] = input & 0xff;
                            std::copy(msg.body.begin(), msg.body.end(), std::back_inserter(recv_buf_));
                        }
                    }
                    break;
                case McsMessage::MsgType::KeyMsg2:
                    verify(false);
                    break;
                case McsMessage::MsgType::LoadStartMsg:
                    // It will be dropped because InetBuf is cleared.
                    break;
                case McsMessage::MsgType::LoadEndMsg:
                    for (int i = 0; i < matching_.player_count(); i++) {
                        if (i != matching_.peer_id()) {
                            auto a = McsMessage::Create(McsMessage::MsgType::LoadStartMsg, i);
                            std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
                        }
                    }

                    if (!ggpo::rollbacking()) {
                        ggpo::setExInput(ExInputWaitLoadEnd);
                        NOTICE_LOG(COMMON, "LoadEndMsg KeyFrame:%d", frame);
                    }
                    break;
                default:
                    WARN_LOG(COMMON, "unhandled mcs msg: %s", McsMessage::MsgTypeName(msg.Type()));
                    WARN_LOG(COMMON, "%s", msg.ToHex().c_str());
                    break;
            }

            verify(recv_buf_.size() <= size);
        }

        if (mapleInputState[0].exInput) {
            auto exInput = mapleInputState[0].exInput;
            bool ok = true;
            for (int i = 0; i < matching_.player_count(); i++) {
                ok &= mapleInputState[i].exInput == exInput;
            }
            if (ok && exInput == ExInputWaitStart) {
                NOTICE_LOG(COMMON, "StartMsg Join:%d", frame);
                for (int i = 0; i < matching_.player_count(); i++) {
                    if (i != matching_.peer_id()) {
                        auto start_msg = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
                        std::copy(start_msg.body.begin(), start_msg.body.end(), std::back_inserter(recv_buf_));
                    }
                }
                if (!ggpo::rollbacking()) {
                    ggpo::setExInput(ExInputNone);
                }
            }
            if (ok && exInput == ExInputWaitLoadEnd) {
                NOTICE_LOG(COMMON, "LoadEndMsg Join:%d", frame);
                for (int i = 0; i < matching_.player_count(); i++) {
                    if (i != matching_.peer_id()) {
                        auto b = McsMessage::Create(McsMessage::MsgType::LoadEndMsg, i);
                        std::copy(b.body.begin(), b.body.end(), std::back_inserter(recv_buf_));
                    }
                }
                if (!ggpo::rollbacking()) {
                    ggpo::setExInput(ExInputNone);
                }
            }
        }
        verify(recv_buf_.size() <= size);
    }

    if (recv_buf_.empty()) {
        return 0;
    }

    int n = std::min<int>(recv_buf_.size(), size);
    for (int i = 0; i < n; ++i) {
        gdxsv_WriteMem8(addr + i, recv_buf_.front());
        recv_buf_.pop_front();
    }
    return n;
}

u32 GdxsvBackendRollback::OnSockPoll() {
    if (state_ <= State::LbsStartBattleFlow) {
        ProcessLbsMessage();
    }
    if (0 < recv_delay_) {
        recv_delay_--;
        return 0;
    }

    return recv_buf_.size();
}

void GdxsvBackendRollback::ProcessLbsMessage() {
    if (state_ == State::StartLocalTest) {
        LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
        recv_delay_ = 1;
        state_ = State::LbsStartBattleFlow;
    }

    LbsMessage msg;
    if (lbs_tx_reader_.Read(msg)) {
        NOTICE_LOG(COMMON, "%s", msg.to_hex().c_str());
        if (state_ == State::StartLocalTest) {
            state_ = State::LbsStartBattleFlow;
        }

        if (msg.command == LbsMessage::lbsLobbyMatchingEntry) {
            LbsMessage::SvAnswer(msg).Serialize(recv_buf_);
            LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskMatchingJoin) {
            LbsMessage::SvAnswer(msg).Write8(matching_.player_count())->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskPlayerSide) {
            LbsMessage::SvAnswer(msg).Write8(matching_.peer_id() + 1)->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskPlayerInfo) {
            int pos = msg.Read8();
            DummyGameParam[16] = '0' + pos;
            DummyGameParam[17] = 0;
            LbsMessage::SvAnswer(msg)
                .Write8(pos)
                ->WriteString("USER0" + std::to_string(pos))
                ->WriteString("USER0" + std::to_string(pos))
                ->WriteBytes(reinterpret_cast<char *>(DummyGameParam), sizeof(DummyGameParam))
                ->Write16(1)
                ->Write16(0)
                ->Write16(0)
                ->Write16(0)
                ->Write16(0)
                ->Write16(0)
                ->Write16(1 + (pos - 1) / 2)
                ->Write16(0)
                ->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskRuleData) {
            LbsMessage::SvAnswer(msg).WriteBytes((char *)DummyRuleData, sizeof(DummyRuleData))->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskBattleCode) {
            LbsMessage::SvAnswer(msg).WriteString("012345")->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskMcsVersion) {
            LbsMessage::SvAnswer(msg).Write8(10)->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsAskMcsAddress) {
            LbsMessage::SvAnswer(msg)
                .Write16(4)
                ->Write8(255)
                ->Write8(255)
                ->Write8(255)
                ->Write8(255)
                ->Write16(2)
                ->Write16(255)
                ->Serialize(recv_buf_);
        }

        if (msg.command == LbsMessage::lbsLogout) {
            state_ = State::McsWaitJoin;
        }

        recv_delay_ = 1;
    }
}

void GdxsvBackendRollback::ApplyPatch(bool first_time) {
    if (state_ == State::None || state_ == State::End) {
        return;
    }

    gdxsv.WritePatch();

    if (gdxsv.Disk() == 2) {
        // Skip Key MsgPush
        gdxsv_WriteMem16(0x8c045f64, 9);
        gdxsv_WriteMem8(0x0c3abb90, 1);
    }
}

void GdxsvBackendRollback::RestorePatch() {
    if (gdxsv.Disk() == 2) {
        gdxsv_WriteMem16(0x8c045f64, 0x410b);
        gdxsv_WriteMem8(0x0c3abb90, 2);
    }
}
