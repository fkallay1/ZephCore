#include "dispatcher_fixture.h"
#include <app/KissModem.h>
#include <deque>
#include <vector>

// KissModem (app/KissModem.cpp) against a scripted host stream and radio: KISS
// framing both ways, and the ZephCore changes to its TX state machine (LBT
// refusal back to CSMA, timeout only once the radio has concluded, SetRadio range
// check, msUntilNextTx()).

static uint32_t kiss_now = 0;
extern "C" uint32_t k_uptime_get_32() { return kiss_now; }
extern "C" int32_t k_msleep(int32_t ms) { kiss_now += uint32_t(ms); return 0; }

namespace {
struct HostStream : Stream {
    std::deque<uint8_t> in;
    std::vector<uint8_t> out;
    int available() override { return int(in.size()); }
    int read() override {
        if (in.empty()) return -1;
        uint8_t b = in.front(); in.pop_front(); return b;
    }
    int peek() override { return in.empty() ? -1 : in.front(); }
    size_t write(uint8_t c) override { out.push_back(c); return 1; }
    int availableForWrite() override { return 4096; }
};
struct ZeroRng : mesh::RNG {
    void random(uint8_t *dest, size_t sz) override { memset(dest, 0, sz); }
};
struct Board : mesh::MainBoard {
    uint16_t getBattMilliVolts() override { return 4000; }
    const char *getManufacturerName() const override { return "test"; }
    void reboot() override {}
    uint8_t getStartupReason() const override { return 0; }
};

std::vector<uint8_t> escaped(uint8_t type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> f{KISS_FEND};
    auto put = [&](uint8_t b) {
        if (b == KISS_FEND) { f.push_back(KISS_FESC); f.push_back(KISS_TFEND); }
        else if (b == KISS_FESC) { f.push_back(KISS_FESC); f.push_back(KISS_TFESC); }
        else f.push_back(b);
    };
    put(type);
    for (uint8_t b : data) put(b);
    f.push_back(KISS_FEND);
    return f;
}

// Splits the host output into unescaped frames (type byte first).
std::vector<std::vector<uint8_t>> frames(const std::vector<uint8_t>& out) {
    std::vector<std::vector<uint8_t>> result;
    std::vector<uint8_t> cur;
    bool esc = false;
    for (uint8_t b : out) {
        if (b == KISS_FEND) {
            if (!cur.empty()) result.push_back(cur);
            cur.clear(); esc = false;
        } else if (b == KISS_FESC) {
            esc = true;
        } else {
            cur.push_back(esc ? (b == KISS_TFEND ? KISS_FEND : KISS_FESC) : b);
            esc = false;
        }
    }
    return result;
}

struct Modem {
    HostStream host;
    test_support::Radio radio;
    ZeroRng rng;
    Board board;
    SensorManager sensors;
    mesh::LocalIdentity identity;
    KissModem modem{host, identity, rng, radio, board, sensors};

    Modem() { kiss_now = 1000; radio.airtime = 1000; modem.begin(); }
    void send(uint8_t type, const std::vector<uint8_t>& data) {
        for (uint8_t b : escaped(type, data)) host.in.push_back(b);
    }
    // As the event loop: several passes per wake, so every step made due is taken.
    void pass() { for (int i = 0; i < 4; i++) modem.loop(); }
    void advance(uint32_t ms) { kiss_now += ms; pass(); }
    // Result byte of the TX_DONE notification, or -1 if none has been sent.
    int txDone() {
        for (auto& f : frames(host.out)) {
            if (f.size() == 3 && f[0] == KISS_CMD_SETHARDWARE && f[1] == HW_RESP_TX_DONE) return f[2];
        }
        return -1;
    }
    // Runs the modem in slot-sized steps until TX_DONE or `limit` ms have passed.
    int runUntilDone(uint32_t limit) {
        for (uint32_t t = 0; t <= limit && txDone() < 0; t += 10) advance(10);
        return txDone();
    }
};
}

TEST(kiss_framing_round_trip, "UNIT-KISS-001",
     "Host frames are unescaped to the radio and received packets are escaped with RX metadata") {
    Modem m;
    const std::vector<uint8_t> payload{0x01, KISS_FEND, 0x02, KISS_FESC, 0x03};
    m.send(KISS_CMD_TXDELAY, {0});
    m.send(KISS_CMD_DATA, payload);
    m.pass();
    CHECK(m.radio.sent.size() == 1);
    CHECK(m.radio.sent[0] == payload);

    m.host.out.clear();
    m.modem.onPacketReceived(-9, -100, payload.data(), uint16_t(payload.size()));
    m.pass();
    auto out = frames(m.host.out);
    CHECK(out.size() == 2);
    std::vector<uint8_t> data{KISS_CMD_DATA};
    data.insert(data.end(), payload.begin(), payload.end());
    CHECK(out[0] == data);
    CHECK((out[1] == std::vector<uint8_t>{KISS_CMD_SETHARDWARE, HW_RESP_RX_META, uint8_t(-9), uint8_t(-100)}));
    // Every byte between the frame delimiters is escaped: no bare FEND inside a frame.
    CHECK(std::count(m.host.out.begin(), m.host.out.end(), KISS_FEND) == 4);

    // An oversized frame without a closing FEND is dropped and the parser recovers.
    m.host.in.push_back(KISS_FEND);
    for (int i = 0; i < KISS_MAX_FRAME_SIZE + 10; i++) m.host.in.push_back(0x00);
    m.send(KISS_CMD_SETHARDWARE, {HW_CMD_PING});
    m.host.out.clear();
    m.pass();
    out = frames(m.host.out);
    CHECK(out.size() == 1);
    CHECK(out[0][0] == KISS_CMD_SETHARDWARE && out[0][1] == HW_RESP(HW_CMD_PING));
}

TEST(kiss_lbt_refusal_back_to_csma, "UNIT-KISS-002",
     "An LBT refusal returns to CSMA and fails only at the wait-clear bound") {
    Modem m;
    m.radio.accept = false;
    m.send(KISS_CMD_TXDELAY, {0});
    m.send(KISS_CMD_DATA, {0xAA});
    const uint32_t start = kiss_now;
    m.pass();
    CHECK(m.txDone() < 0);
    CHECK(m.modem.msUntilNextTx() == KISS_DEFAULT_SLOTTIME * 10);

    // Bound: 1.5 x the airtime of a full packet (1000 ms in this radio) from the start of CSMA.
    CHECK(m.runUntilDone(2000) == 0x00);
    CHECK(kiss_now - start >= 1500);
    CHECK(kiss_now - start < 1500 + KISS_DEFAULT_SLOTTIME * 10 + 20);
    CHECK(m.radio.attempts > 5);
    CHECK(!m.modem.isTxBusy());

    // The channel clears inside the bound: the frame goes out and completes.
    Modem n;
    n.radio.accept = false;
    n.send(KISS_CMD_TXDELAY, {0});
    n.send(KISS_CMD_DATA, {0xBB});
    n.pass();
    n.advance(300);
    n.radio.accept = true;
    n.runUntilDone(200);
    CHECK(n.radio.sent.size() == 1);
    CHECK(n.modem.isActuallyTransmitting());
    n.radio.completion = true;
    n.pass();
    CHECK(n.txDone() == 0x01);
    CHECK(n.radio.finished == 1);
}

TEST(kiss_tx_timeout_waits_for_radio, "UNIT-KISS-003",
     "A transmit times out only once the radio has concluded it") {
    Modem m;
    m.send(KISS_CMD_TXDELAY, {0});
    m.send(KISS_CMD_DATA, {0xCC});
    m.pass();
    CHECK(m.modem.isActuallyTransmitting());
    CHECK(m.modem.msUntilNextTx() == 1500);

    // Past the airtime bound while the radio still transmits: no verdict, no timed wake.
    m.advance(10000);
    CHECK(m.txDone() < 0);
    CHECK(m.radio.finished == 0);
    CHECK(m.modem.msUntilNextTx() == UINT32_MAX);

    // The radio concludes without a completion: the timeout fires on the next pass.
    m.radio.active = false;
    m.pass();
    CHECK(m.txDone() == 0x00);
    CHECK(m.radio.finished == 1);
    CHECK(!m.modem.isTxBusy());
    CHECK(m.modem.msUntilNextTx() == UINT32_MAX);
}

static float set_freq, set_bw;
static uint8_t set_sf, set_cr;
static int set_calls;
static void on_set_radio(float freq, float bw, uint8_t sf, uint8_t cr) {
    set_freq = freq; set_bw = bw; set_sf = sf; set_cr = cr; set_calls++;
}

TEST(kiss_set_radio_ranges, "UNIT-KISS-004",
     "SetRadio outside the ZephCore preset ranges is refused") {
    Modem m;
    m.modem.setRadioCallback(on_set_radio);
    set_calls = 0;
    auto setRadio = [&](uint32_t hz, uint32_t bw_hz, uint8_t sf, uint8_t cr) {
        std::vector<uint8_t> d{HW_CMD_SET_RADIO};
        for (int i = 0; i < 4; i++) d.push_back(uint8_t(hz >> (8 * i)));
        for (int i = 0; i < 4; i++) d.push_back(uint8_t(bw_hz >> (8 * i)));
        d.push_back(sf); d.push_back(cr);
        m.host.out.clear();
        m.send(KISS_CMD_SETHARDWARE, d);
        m.pass();
        auto out = frames(m.host.out);
        CHECK(out.size() == 1);
        return out[0];
    };
    const std::vector<uint8_t> ok{KISS_CMD_SETHARDWARE, HW_RESP_OK};
    const std::vector<uint8_t> refused{KISS_CMD_SETHARDWARE, HW_RESP_ERROR, HW_ERR_INVALID_PARAM};

    CHECK(setRadio(869525000, 62500, 7, 5) == ok);
    CHECK(set_calls == 1 && set_sf == 7 && set_cr == 5);
    CHECK(set_freq > 869.52f && set_freq < 869.53f && set_bw == 62.5f);

    CHECK(setRadio(100000000, 62500, 7, 5) == refused);   // below 150 MHz
    CHECK(setRadio(869525000, 5000, 7, 5) == refused);    // below 7 kHz
    CHECK(setRadio(869525000, 62500, 4, 5) == refused);   // SF below 5
    CHECK(setRadio(869525000, 62500, 13, 5) == refused);  // SF above 12
    CHECK(setRadio(869525000, 62500, 7, 9) == refused);   // CR above 4/8
    CHECK(set_calls == 1);
}

TEST(kiss_wake_deadlines, "UNIT-KISS-005",
     "msUntilNextTx reports each timed state and nothing while idle") {
    Modem m;
    CHECK(m.modem.msUntilNextTx() == UINT32_MAX);

    // Default TXDELAY (50 x 10 ms) after a clear channel.
    m.send(KISS_CMD_DATA, {0xDD});
    m.pass();
    CHECK(m.modem.msUntilNextTx() == KISS_DEFAULT_TXDELAY * 10);
    m.advance(200);
    CHECK(m.modem.msUntilNextTx() == KISS_DEFAULT_TXDELAY * 10 - 200);
    m.advance(KISS_DEFAULT_TXDELAY * 10 - 200);
    CHECK(m.modem.isActuallyTransmitting());

    // Busy channel: rechecked once per slot.
    Modem n;
    n.radio.receiving = true;
    n.send(KISS_CMD_DATA, {0xEE});
    n.pass();
    CHECK(n.modem.msUntilNextTx() == KISS_DEFAULT_SLOTTIME * 10);
    n.advance(30);
    CHECK(n.modem.msUntilNextTx() == KISS_DEFAULT_SLOTTIME * 10 - 30);
}
