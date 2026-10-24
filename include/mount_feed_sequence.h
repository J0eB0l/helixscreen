// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file mount_feed_sequence.h
 * @brief Loading or unloading filament on a machine that has to mount the tool
 *        to feed it.
 *
 * A hotend changer with one extruder drive (MedusaHC) can only move filament
 * through the tool on the carriage. A Load there is one operation of named
 * stages: ask the operator to insert the strand into the hot end while the
 * tool is still docked (on the carriage the extruder's gears are in the way),
 * mount the tool, heat it, draw the strand in slowly and purge, then put the
 * carriage back as it was. An Unload mounts, heats, retracts the strand fully
 * out and restores the carriage.
 *
 * Everything here is plain values and gcode text, so the sequence is testable
 * with no printer. The backend sends each stage, advances on its ack, and
 * waits on the operator's reply before the first mount. The prompt is
 * Klipper's own action prompt, raised by the stage's RESPOND lines, so it shows
 * on every screen attached to the printer; its buttons answer with a RESPOND
 * line parse_reply() recognises.
 */

#pragma once

#include "ams_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::mount_feed {

enum class Step : uint8_t { Mount, Heat, Insert, Purge, Retract, Restore };

/// The steps one tool's Load or Unload shows, in order. Mount and Restore are
/// skipped when the tool is already on the carriage.
[[nodiscard]] std::vector<Step> steps(bool load);

/// Untranslated label for @p step; the caller passes it through lv_tr().
[[nodiscard]] const char* step_label(Step step);

/// The coarse action @p step projects to.
[[nodiscard]] AmsAction step_action(Step step);

/// One tool's piece of a run: what the backend read off that tool at dispatch.
struct HeadRequest {
    int head = -1;
    int temp_c = 0;
    std::string mount_gcode; ///< Mounts @ref head; empty when it is already mounted
    /// The tool's heater target right now. The next-tool preheat must never
    /// LOWER a target, so a tool already set hotter than the preheat value is
    /// left alone instead of being pulled back to it.
    int heater_target_c = 0;
};

/// How long the operator prompt may wait before the run unwinds itself: a run
/// nobody is attending must not go on to mount and heat a tool.
constexpr uint32_t kPromptTimeoutMs = 180'000;

/// What one Load or Unload needs to know, read off the backend at dispatch.
struct Request {
    bool load = true;
    /// The tools the run works, in order. One entry for a plain Load or
    /// Unload; several for Load all / Unload all.
    std::vector<HeadRequest> heads;
    std::string restore_gcode; ///< Puts the carriage back; empty when nothing changes
    /// The tool restore_gcode puts on the carriage, or -1 when it parks.
    int restore_head = -1;
    /// The prompt may not outlive this. A Load asks once, up front, for every
    /// strand.
    uint32_t prompt_timeout_ms = kPromptTimeoutMs;
    /// The prompt fields are ENGLISH: they travel inside RESPOND lines, which
    /// every client attached to the printer shows verbatim, so no locale of
    /// ours may decide what Mainsail displays.
    std::string prompt_title;
    std::string prompt_text;
    std::string continue_label;
    std::string cancel_label;
};

struct Stage {
    Step step = Step::Mount;
    std::string gcode;
    /// The stage raises the operator prompt: once it is sent, the run waits
    /// for a Reply before going on.
    bool awaits_operator = false;
    /// Position of this stage's tool in Request::heads; the shared prompt
    /// counts as the first tool's and Restore as the last one's.
    int tool_index = 0;
    /// The stage swaps the carriage: a Mount, a Restore, or a cleanup that puts
    /// the carriage back. The dock sensors read -2 for a moment mid-swap.
    bool swaps = false;
};

/// The stages @p request runs, in order.
[[nodiscard]] std::vector<Stage> plan(const Request& request);

/// The heater object Klipper multi-extruder naming gives @p head's tool: the
/// first extruder is plain `extruder`, the rest `extruder<n>`.
[[nodiscard]] std::string heater_name(int head);

/// The RESPOND lines that raise the operator prompt.
[[nodiscard]] std::string prompt_gcode(const Request& request);

/// The RESPOND line that closes it.
[[nodiscard]] std::string prompt_end_gcode();

enum class Reply : uint8_t { None, Continue, Cancel };

/// The operator's answer, when @p line is the console echo of a prompt button.
[[nodiscard]] Reply parse_reply(std::string_view line);

/// Whether a console line could be a reply, cheap enough to run on every line.
[[nodiscard]] bool may_be_reply(std::string_view line);

/// What one console line says about the operator prompt. Dismissed is the
/// echo of `action:prompt_end` — the prompt is gone from every client, so the
/// operator can no longer answer it.
enum class ConsoleSignal : uint8_t { None, Continue, Cancel, Dismissed };

/// parse_reply() plus the prompt-dismissal echo, for the watcher that sees
/// every console line.
[[nodiscard]] ConsoleSignal parse_console(std::string_view line);

/// Whether @p line is one the prompt watcher must look at closer.
[[nodiscard]] bool may_be_prompt_signal(std::string_view line);

/**
 * @brief One Load or Unload in progress.
 *
 * Advances on the backend's acks and the operator's reply. Any failure, or a
 * Cancel, turns the rest of the run into the cleanup: prompt closed and, once
 * anything past the prompt has gone out, heaters off and carriage restored.
 * The cleanup runs once; a failure inside it ends the run.
 */
class Run {
  public:
    explicit Run(Request request);

    [[nodiscard]] const Request& request() const {
        return request_;
    }

    /// The gcode the current stage sends, or nullopt once the run is over.
    [[nodiscard]] std::optional<std::string> current_gcode() const;

    /// Position of the current stage in steps(load), within the current tool.
    [[nodiscard]] int step_index() const;
    /// Position in the whole run's step bar: with several tools, each tool owns
    /// one strides'-worth of the bar, so the index never restarts mid-run.
    [[nodiscard]] int phase_index() const;
    [[nodiscard]] Step current_step() const;
    /// The stage in flight, or nullptr once the run is over.
    [[nodiscard]] const Stage* current_stage() const;

    /// The prompt is up and the run is waiting for the operator.
    [[nodiscard]] bool awaiting_operator() const {
        return awaiting_operator_;
    }
    [[nodiscard]] bool finished() const;
    /// The run ended in its cleanup rather than completing.
    [[nodiscard]] bool aborted() const {
        return cleaning_up_;
    }
    /// The tools whose work fully finished before the run stopped. Empty while
    /// the run is still on its first tool, and after a clean completion the
    /// caller reports success rather than a partial list.
    [[nodiscard]] std::vector<int> completed_heads() const;

    /// The current stage's gcode was acked.
    void stage_done();
    /// The operator answered the prompt. Ignored unless the run is waiting.
    void operator_reply(Reply reply);
    /// The current stage failed, or the user cancelled. @p restore_carriage
    /// false leaves the carriage alone, for a carriage nothing can vouch for.
    void abort(bool restore_carriage = true);
    /// What advancing the operator wait by @p elapsed_ms did. Expired also
    /// aborts the run into its cleanup.
    enum class WaitResult : uint8_t { Waiting, NotWaiting, Expired };
    [[nodiscard]] WaitResult advance_wait(uint32_t elapsed_ms);

  private:
    Request request_;
    std::vector<Stage> stages_;
    size_t index_ = 0;
    bool awaiting_operator_ = false;
    bool cleaning_up_ = false;
    uint32_t waited_ms_ = 0;
};

} // namespace helix::mount_feed
