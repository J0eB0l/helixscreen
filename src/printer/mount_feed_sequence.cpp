// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_feed_sequence.h"

#include "klipper_extruder_naming.h"
#include "macro_param_cache.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

namespace helix::mount_feed {

namespace {

constexpr std::string_view kContinueMarker = "HELIX_MOUNT_FEED_CONTINUE";
constexpr std::string_view kCancelMarker = "HELIX_MOUNT_FEED_CANCEL";

/// Text safe inside a RESPOND MSG="..." and an action-prompt field: a double
/// quote ends the MSG, `;` starts a gcode comment, `|` splits a button's fields.
std::string respond_safe(std::string text) {
    for (char& c : text) {
        if (c == '"') {
            c = '\'';
        } else if (c == ';') {
            c = ',';
        } else if (c == '|') {
            c = '/';
        } else if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return text;
}

std::string respond_command(std::string_view message) {
    return fmt::format("RESPOND TYPE=command MSG=\"{}\"", message);
}

/// Set @p head's own heater by name. Every temperature the run sends goes this
/// way: a changer that swaps tools without ACTIVATE_EXTRUDER leaves Klipper's
/// active extruder where it was, so M104/M109 would act on that one instead.
std::string heater_target_gcode(int head, int target_c) {
    return fmt::format("SET_HEATER_TEMPERATURE HEATER={} TARGET={}", heater_name(head), target_c);
}

/// Warm @p head partway while another tool works, so its own heat stage starts
/// from heat instead of room temperature - unless it is already set at least
/// that hot, in which case its target stands and nothing is sent.
std::string preheat_gcode(const HeadRequest& head) {
    const int preheat = head.temp_c * 65 / 100;
    if (head.heater_target_c >= preheat) {
        return {};
    }
    return heater_target_gcode(head.head, preheat);
}

std::string heater_off_gcode(int head) {
    return heater_target_gcode(head, 0);
}

/// Heat @p head and wait for it, by name rather than through a printer's own
/// M109 override, which may wait on a different heater than it set.
std::string heat_gcode(int head, int temp_c) {
    return heater_target_gcode(head, temp_c) +
           fmt::format("\nTEMPERATURE_WAIT SENSOR={} MINIMUM={}", heater_name(head), temp_c - 2);
}

/// @p gcode, then @p head's heater off by name, for the same reason.
std::string with_heater_off(std::string gcode, int head) {
    return gcode + "\n" + heater_off_gcode(head);
}

/// The moves that work one tool's filament: the printer's own macro when it
/// takes the tool, else the built-in slow draw and purge (or the full retract).
std::string feed_or_retract_gcode(const Request& r, const HeadRequest& head) {
    if (r.load) {
        return r.macro.empty() ? "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 F1800"
                               : fmt::format("{} T={} TEMP={}", r.macro, head.head, head.temp_c);
    }
    return r.macro.empty() ? "M83\nG1 E-25 F1800\nG1 E-150 F1200"
                           : fmt::format("{} T={}", r.macro, head.head);
}

} // namespace

std::string heater_name(int head) {
    return helix::extruder_name_for_tool(head);
}

std::vector<Step> steps(bool load) {
    if (load) {
        return {Step::Insert, Step::Mount, Step::Heat, Step::Purge, Step::Restore};
    }
    return {Step::Mount, Step::Heat, Step::Retract, Step::Restore};
}

const char* step_label(Step step) {
    switch (step) {
    case Step::Mount:
        return "Mount tool";
    case Step::Heat:
        return "Heat nozzle";
    case Step::Insert:
        return "Insert filament";
    case Step::Purge:
        return "Purge";
    case Step::Retract:
        return "Retract filament";
    case Step::Restore:
        return "Restore tool";
    }
    return "";
}

AmsAction step_action(Step step) {
    switch (step) {
    case Step::Mount:
    case Step::Restore:
        return AmsAction::SELECTING;
    case Step::Heat:
        return AmsAction::HEATING;
    case Step::Insert:
        return AmsAction::LOADING;
    case Step::Purge:
        return AmsAction::PURGING;
    case Step::Retract:
        return AmsAction::UNLOADING;
    }
    return AmsAction::LOADING;
}

std::string prompt_gcode(const Request& r) {
    std::string out;
    out += respond_command("action:prompt_begin " + respond_safe(r.prompt_title)) + "\n";
    out += respond_command("action:prompt_text " + respond_safe(r.prompt_text)) + "\n";
    out += respond_command(fmt::format("action:prompt_footer_button {}|RESPOND TYPE=command "
                                       "MSG={}|error",
                                       respond_safe(r.cancel_label), kCancelMarker)) +
           "\n";
    out += respond_command(fmt::format("action:prompt_footer_button {}|RESPOND TYPE=command "
                                       "MSG={}|primary",
                                       respond_safe(r.continue_label), kContinueMarker)) +
           "\n";
    out += respond_command("action:prompt_show");
    return out;
}

std::string prompt_end_gcode() {
    return respond_command("action:prompt_end");
}

std::vector<Stage> plan(const Request& r) {
    std::vector<Stage> stages;
    // Whichever stage follows an operator-wait stage closes the prompt: the
    // reply moved the run on, and the dialog is still up until this lands.
    bool close_prompt = false;
    const auto push_stage = [&](Step step, std::string gcode, bool awaits, int tool) {
        if (close_prompt) {
            gcode = prompt_end_gcode() + "\n" + std::move(gcode);
            close_prompt = false;
        }
        const bool swaps = step == Step::Mount || step == Step::Restore;
        stages.push_back({step, std::move(gcode), awaits, tool, swaps});
        if (awaits) {
            close_prompt = true;
        }
    };
    if (r.load) {
        // One prompt for every strand, before anything mounts: a strand goes
        // into the docked hot end, since on the carriage the extruder's gears
        // stand in the way. The run then works tool by tool.
        push_stage(Step::Insert, prompt_gcode(r), /*awaits_operator=*/true, 0);
    }
    for (size_t t = 0; t < r.heads.size(); ++t) {
        const HeadRequest& head = r.heads[t];
        const HeadRequest* next = t + 1 < r.heads.size() ? &r.heads[t + 1] : nullptr;
        const std::string preheat = next ? preheat_gcode(*next) : std::string();
        const int tool = static_cast<int>(t);
        std::string heat = heat_gcode(head.head, head.temp_c);
        if (!preheat.empty()) {
            // The next tool warms partway while this one works. It rides the
            // stage that starts this tool: mount where there is one, else heat.
            if (head.mount_gcode.empty()) {
                heat += "\n" + preheat;
            }
        }
        if (!head.mount_gcode.empty()) {
            push_stage(Step::Mount,
                       preheat.empty() ? head.mount_gcode : head.mount_gcode + "\n" + preheat,
                       false, tool);
        }
        push_stage(Step::Heat, std::move(heat), false, tool);
        push_stage(r.load ? Step::Purge : Step::Retract,
                   with_heater_off(feed_or_retract_gcode(r, head), head.head), false, tool);
    }
    if (!r.restore_gcode.empty()) {
        push_stage(Step::Restore, r.restore_gcode, false, static_cast<int>(r.heads.size()) - 1);
    }
    return stages;
}

bool may_be_reply(std::string_view line) {
    return line.find("HELIX_MOUNT_FEED_") != std::string_view::npos;
}

Reply parse_reply(std::string_view line) {
    switch (parse_console(line)) {
    case ConsoleSignal::Continue:
        return Reply::Continue;
    case ConsoleSignal::Cancel:
        return Reply::Cancel;
    case ConsoleSignal::None:
    case ConsoleSignal::Dismissed:
        break;
    }
    return Reply::None;
}

bool may_be_prompt_signal(std::string_view line) {
    return may_be_reply(line) || line.find("action:prompt_end") != std::string_view::npos;
}

ConsoleSignal parse_console(std::string_view line) {
    // The console echo of `RESPOND TYPE=command MSG=<marker>` is "// <marker>".
    // Exact match only: the prompt's own button lines carry the marker text too.
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };
    line = trim(line);
    if (line.substr(0, 2) == "//") {
        line = trim(line.substr(2));
    }
    if (line == kContinueMarker) {
        return ConsoleSignal::Continue;
    }
    if (line == kCancelMarker) {
        return ConsoleSignal::Cancel;
    }
    if (line == "action:prompt_end") {
        return ConsoleSignal::Dismissed;
    }
    return ConsoleSignal::None;
}

bool macro_takes_tool(const CachedMacroInfo& macro) {
    if (macro.knowledge != MacroParamKnowledge::KNOWN_PARAMS) {
        return false;
    }
    return std::any_of(macro.params.begin(), macro.params.end(),
                       [](const MacroParam& p) { return p.name == "T"; });
}

Run::Run(Request request) : request_(std::move(request)), stages_(plan(request_)) {}

std::optional<std::string> Run::current_gcode() const {
    if (finished() || awaiting_operator_) {
        return std::nullopt;
    }
    return stages_[index_].gcode;
}

bool Run::finished() const {
    return index_ >= stages_.size();
}

Step Run::current_step() const {
    return finished() ? Step::Restore : stages_[index_].step;
}

int Run::step_index() const {
    const auto all = steps(request_.load);
    const auto it = std::find(all.begin(), all.end(), current_step());
    return it == all.end() ? -1 : static_cast<int>(it - all.begin());
}

int Run::phase_index() const {
    const int within = step_index();
    if (within < 0) {
        return -1;
    }
    const int tool = finished() ? static_cast<int>(request_.heads.size()) - 1
                                : std::max(stages_[index_].tool_index, 0);
    return tool * static_cast<int>(steps(request_.load).size()) + within;
}

std::vector<int> Run::completed_heads() const {
    std::vector<int> done;
    if (cleaning_up_ || request_.heads.size() < 2) {
        return done;
    }
    // A tool is done once the run has moved past its last WORK stage: the
    // carriage restore that follows the last tool belongs to the whole run,
    // so a tool being put back counts as complete, not in flight.
    std::vector<size_t> last_work(request_.heads.size(), 0);
    for (size_t i = 0; i < stages_.size(); ++i) {
        const Stage& s = stages_[i];
        if (s.step == Step::Restore || s.tool_index < 0 ||
            static_cast<size_t>(s.tool_index) >= last_work.size()) {
            continue;
        }
        last_work[static_cast<size_t>(s.tool_index)] = i;
    }
    for (size_t t = 0; t < last_work.size(); ++t) {
        if (last_work[t] < index_) {
            done.push_back(request_.heads[t].head);
        }
    }
    return done;
}

void Run::stage_done() {
    if (finished() || awaiting_operator_) {
        return;
    }
    if (stages_[index_].awaits_operator && !cleaning_up_) {
        awaiting_operator_ = true;
        waited_ms_ = 0;
        return;
    }
    ++index_;
}

void Run::operator_reply(Reply reply) {
    if (!awaiting_operator_ || reply == Reply::None) {
        return;
    }
    awaiting_operator_ = false;
    if (reply == Reply::Continue) {
        ++index_;
        return;
    }
    abort();
}

Run::WaitResult Run::advance_wait(uint32_t elapsed_ms) {
    if (!awaiting_operator_) {
        return WaitResult::NotWaiting;
    }
    waited_ms_ += elapsed_ms;
    if (waited_ms_ < request_.prompt_timeout_ms) {
        return WaitResult::Waiting;
    }
    abort();
    return WaitResult::Expired;
}

const Stage* Run::current_stage() const {
    return finished() ? nullptr : &stages_[index_];
}

void Run::abort(bool restore_carriage) {
    if (cleaning_up_ || finished()) {
        index_ = stages_.size();
        return;
    }
    // A prompt still open is closed. Once anything past the prompt has gone
    // out, every heater in the plan goes off BY NAME (the active extruder
    // before the first mount is the PREVIOUS tool's, and a blanket M104 S0
    // would zero a target its user set) and the carriage goes back where it
    // was. Before that nothing has moved or heated, so there is nothing to undo.
    const bool started = std::any_of(stages_.begin(), stages_.begin() + index_ + 1,
                                     [](const Stage& s) { return !s.awaits_operator; });
    std::string cleanup;
    if (awaiting_operator_ || current_step() == Step::Insert) {
        cleanup = prompt_end_gcode();
    }
    bool swaps = false;
    if (started) {
        for (const HeadRequest& head : request_.heads) {
            if (!cleanup.empty()) {
                cleanup += "\n";
            }
            cleanup += heater_off_gcode(head.head);
        }
        if (restore_carriage && !request_.restore_gcode.empty()) {
            cleanup += "\n" + request_.restore_gcode;
            swaps = true;
        }
    }
    awaiting_operator_ = false;
    cleaning_up_ = true;
    stages_ = {Stage{Step::Restore, std::move(cleanup), false, 0, swaps}};
    index_ = 0;
    if (stages_.front().gcode.empty()) {
        index_ = stages_.size();
    }
}

} // namespace helix::mount_feed
