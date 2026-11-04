// helper_protocol.hpp — how the GUI talks to kiln-helper, the only part of Kiln
// that runs privileged (Linux: root through pkexec; Windows: elevated through
// UAC).
//
// Request: argv. `kiln-helper write <device> <image> [options]`, or `kiln-helper
// ventoy <device> [options]` (a Ventoy disk: no image), options named as the CLI's. argv needs no escaping on Linux (paths may hold spaces) and
// pkexec passes it through untouched; on Windows the GUI quotes it for
// CommandLineToArgvW and puts `--pipe NAME` first (where the lines below go).
//
// Reply: lines on stdout (Windows: the pipe), one event each:
//   stage <stage> <done> <total>     progress (stage names: stage_key())
//   msg <text>                       something to show in the log
//   result ok <direct 0|1> <write_s> <verify_s> <sha256 or ->
//   result mismatch <text>           verification found a difference
//   result cancelled
//   result error <text>
// stdin (Windows: the pipe): a line "cancel" stops the write (so does closing it).
#pragma once

#include "unattend.hpp"
#include "ventoy.hpp"
#include "writer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace kiln {

struct HelperRequest {
    std::string device;  // "sdc"
    std::string image;   // as the user gave it; empty for a Ventoy disk
    bool ventoy = false; // make a Ventoy disk instead of writing an image
    VentoySetup ventoy_setup;
    bool verify = true;
    bool raw = false;    // a Windows ISO written raw instead of built
    bool force = false;  // a raw write that will not boot
    std::string temp_dir;  // Windows: the user's temp directory (refused on Linux: root picks its own)
    Customization custom;
};

std::vector<std::string> helper_args(const HelperRequest& r);  // after the program name
// Arguments as one Windows command line, quoted so that CommandLineToArgvW (and
// the C runtime's argv) give them back unchanged.
std::string quote_windows_args(const std::vector<std::string>& args);
HelperRequest parse_helper_args(const std::vector<std::string>& args);  // throws std::invalid_argument

const char* stage_key(Stage s);
std::optional<Stage> stage_from_key(const std::string& key);

struct HelperEvent {
    enum class Kind { stage, msg, ok, mismatch, cancelled, error } kind = Kind::msg;
    Progress progress;  // stage
    std::string text;   // msg, mismatch, error
    bool direct = false;
    double write_seconds = 0, verify_seconds = 0;
    std::string sha256;  // ok, raw writes only
};

std::string format_event(const HelperEvent& e);  // one line, no newline; text is flattened to one line
std::optional<HelperEvent> parse_event(const std::string& line);

}  // namespace kiln
