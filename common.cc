//          Copyright Jean Pierre Cimalando 2018.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "common.h"
#include <thread>
#include <chrono>
#include <stdexcept>
#include <string.h>
#include <stdio.h>
#include <assert.h>
namespace stc = std::chrono;

void *player = nullptr;
Player_Type player_type = Player_Type::OPL3;
int16_t *buffer = nullptr;
DcFilter dcfilter[2];
VuMonitor lvmonitor[2];
double lvcurrent[2] = {};

void generic_usage(const char *progname, const char *more_options)
{
    fprintf(stderr, "Usage: %s [-p player] [-n num-chips] [-b bank.wopl] [-e emulator]%s\n", progname, more_options);

    fprintf(stderr, "Available players:\n");
    for (Player_Type pt : all_player_types) {
        fprintf(stderr, "   * %s\n", player_name(pt));
    }

    for (Player_Type pt : all_player_types) {
        std::vector<std::string> emus = enumerate_emulators(pt);
        size_t emu_count = emus.size();
        fprintf(stderr, "Available emulators for %s:\n", player_name(pt));
        for (size_t i = 0; i < emu_count; ++i)
            fprintf(stderr, "   * %zu: %s\n", i, emus[i].c_str());
    }
}

template <Player_Type Pt>
void generic_initialize_player(unsigned sample_rate, unsigned nchip, const char *bankfile, int emulator)
{
    typedef Player_Traits<Pt> Traits;
    typedef typename Traits::player Player;

    fprintf(stderr, "%s version %s\n", Traits::name(), Traits::version());

    Player *player = Traits::init(sample_rate);
    ::player = player;
    if (!player)
        throw std::runtime_error("error instantiating ADLMIDI");

    if (emulator >= 0)
        Traits::switch_emulator(player, emulator);

    fprintf(stderr, "Using emulator \"%s\"\n", Traits::emulator_name(player));

    if (!bankfile) {
        fprintf(stderr, "Using default banks.\n");
    }
    else {
        if (Traits::open_bank_file(player, bankfile) < 0)
            throw std::runtime_error("error loading bank file");
        fprintf(stderr, "Using banks from WOPL file.\n");
        Traits::reset(player);  // not sure if necessary
    }

    if (Traits::set_num_chips(player, nchip) < 0)
        throw std::runtime_error("error setting the number of chips");

    fprintf(stderr, "DC filter @ %f Hz, LV monitor @ %f ms\n", dccutoff, lvrelease * 1e3);
    for (unsigned i = 0; i < 2; ++i) {
        dcfilter[i].cutoff(dccutoff / sample_rate);
        lvmonitor[i].release(lvrelease * sample_rate);
    }
}

template <Player_Type Pt>
void generic_player_ready()
{
    typedef Player_Traits<Pt> Traits;
    typedef typename Traits::player Player;

    Player *player = reinterpret_cast<Player *>(::player);

    fprintf(stderr, "%s ready with %u chips.\n",
            Traits::name(), Traits::get_num_chips(player));
}

template <Player_Type Pt>
void generic_play_midi(const uint8_t *msg, unsigned len)
{
    typedef Player_Traits<Pt> Traits;
    typedef typename Traits::player Player;

    Player *player = reinterpret_cast<Player *>(::player);

    if (len <= 0)
        return;

    uint8_t status = msg[0];
    if ((status & 0xf0) == 0xf0)
        return;

    uint8_t channel = status & 0x0f;
    switch (status >> 4) {
    case 0b1001:
        if (len < 3) break;
        if (msg[2] != 0) {
            Traits::rt_note_on(player, channel, msg[1], msg[2]);
            break;
        }
    case 0b1000:
        if (len < 3) break;
        Traits::rt_note_off(player, channel, msg[1]);
        break;
    case 0b1010:
        if (len < 3) break;
        Traits::rt_note_aftertouch(player, channel, msg[1], msg[2]);
        break;
    case 0b1101:
        if (len < 2) break;
        Traits::rt_channel_aftertouch(player, channel, msg[1]);
        break;
    case 0b1011:
        if (len < 3) break;
        Traits::rt_controller_change(player, channel, msg[1], msg[2]);
        break;
    case 0b1100:
        if (len < 2) break;
        Traits::rt_program_change(player, channel, msg[1]);
        break;
    case 0b1110:
        if (len < 3) break;
        Traits::rt_pitchbend_ml(player, channel, msg[2], msg[1]);
        break;
    }
}

template <Player_Type Pt>
void generic_generate_outputs(float *left, float *right, unsigned nframes, unsigned stride)
{
    typedef Player_Traits<Pt> Traits;
    typedef typename Traits::player Player;

    Player *player = reinterpret_cast<Player *>(::player);

    if (nframes <= 0)
        return;

    int16_t *pcm = ::buffer;
    Traits::generate(player, 2 * nframes, pcm);

    DcFilter &dclf = dcfilter[0];
    DcFilter &dcrf = dcfilter[1];
    double lvcurrent[2];

    for (unsigned i = 0; i < nframes; ++i) {
        constexpr double outputgain = 1.0; // 3.5;
        double left_sample = dclf.process(pcm[2 * i] * (outputgain / 32768));
        double right_sample = dcrf.process(pcm[2 * i + 1] * (outputgain / 32768));
        lvcurrent[0] = lvmonitor[0].process(left_sample);
        lvcurrent[1] = lvmonitor[1].process(right_sample);
        left[i * stride] = left_sample;
        right[i * stride] = right_sample;
    }

    ::lvcurrent[0] = lvcurrent[0];
    ::lvcurrent[1] = lvcurrent[1];
}

template <Player_Type Pt>
const char *generic_player_name()
{
    typedef Player_Traits<Pt> Traits;

    return Traits::name();
}

template <Player_Type Pt>
std::vector<std::string> generic_enumerate_emulators()
{
    typedef Player_Traits<Pt> Traits;
    typedef typename Traits::player Player;

    Player *player = Traits::init(44100);
    std::vector<std::string> names;
    for (unsigned i = 0; Traits::switch_emulator(player, i) == 0; ++i)
        names.push_back(Traits::emulator_name(player));
    Traits::close(player);
    return names;
}

#define PLAYER_DISPATCH_CASE(x, fn, ...)                \
    case Player_Type::x:                                \
    return generic_##fn<Player_Type::x>(__VA_ARGS__);   \

#define PLAYER_DISPATCH(type, fn, ...)                                  \
    switch ((type)) {                                                   \
    EACH_PLAYER_TYPE(PLAYER_DISPATCH_CASE, fn, ##__VA_ARGS__)           \
    default: assert(false);                                             \
    }

void initialize_player(unsigned sample_rate, unsigned nchip, const char *bankfile, int emulator)
{
    PLAYER_DISPATCH(::player_type, initialize_player, sample_rate, nchip, bankfile, emulator);
}

void player_ready()
{
    PLAYER_DISPATCH(::player_type, player_ready);
}

void play_midi(const uint8_t *msg, unsigned len)
{
    PLAYER_DISPATCH(::player_type, play_midi, msg, len);
}

void generate_outputs(float *left, float *right, unsigned nframes, unsigned stride)
{
    PLAYER_DISPATCH(::player_type, generate_outputs, left, right, nframes, stride);
}

std::vector<std::string> enumerate_emulators()
{
    PLAYER_DISPATCH(::player_type, enumerate_emulators);
}

const char *player_name(Player_Type pt)
{
    PLAYER_DISPATCH(pt, player_name);
}

Player_Type player_by_name(const char *name)
{
    for (Player_Type pt : all_player_types)
        if (!strcmp(name, player_name(pt)))
            return pt;
    return (Player_Type)-1;
}

std::vector<std::string> enumerate_emulators(Player_Type pt)
{
    PLAYER_DISPATCH(pt, enumerate_emulators);
}

static void print_volume_bar(FILE *out, unsigned size, double vol)
{
    if (size < 2)
        return;
    fprintf(out, "[");
    for (unsigned i = 0; i < size; ++i)
        fprintf(out, "%c", (vol > (double)i / size) ? '*' : '-');
    fprintf(out, "]");
}

void interface_exec()
{
    while (1) {
        fprintf(stderr, "\033[2K");
        double volumes[2] = {lvcurrent[0], lvcurrent[1]};
        const char *names[2] = {"Left", "Right"};

        // enables logarithmic view for perceptual volume, otherwise linear.
        //  (better use linear to watch output for clipping)
        const bool logarithmic = false;

        for (unsigned channel = 0; channel < 2; ++channel) {
            double vol = volumes[channel];
            if (logarithmic && vol > 0) {
                double db = 20 * log10(vol);
                const double dbmin = -60.0;
                vol = (db - dbmin) / (0 - dbmin);
            }
            fprintf(stderr, " %c ", names[channel][0]);
            print_volume_bar(stderr, 30, vol);
            fprintf(stderr, (vol > 1.0) ? " \033[7mCLIP\033[0m" : "     ");
        }

        fprintf(stderr, "\r");
        fflush(stderr);

        std::this_thread::sleep_for(stc::milliseconds(50));
    }
}
