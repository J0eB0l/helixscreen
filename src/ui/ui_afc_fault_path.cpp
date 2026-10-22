// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_afc_fault_path.h"

#include "afc_fault_position.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

/// Where the filament stopped, as a PathSegment. 0 (NONE) hides the graphic.
lv_subject_t g_fault_segment{};
SubjectManager g_subjects;
bool g_initialized = false;

void afc_fault_path_deinit() {
    if (!g_initialized) {
        return;
    }
    g_subjects.deinit_all();
    g_initialized = false;
    spdlog::debug("[AfcFaultPath] Subject deinitialized");
}

/// Publishing does not depend on afc_fault_path_register() having run first. A
/// modal built while the name is unregistered binds nothing: the strip shows with
/// all of its captions.
void ensure_subject() {
    if (g_initialized) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(g_fault_segment, 0, "afc_fault_segment", g_subjects);
    g_initialized = true;

    // Self-register cleanup — the subject must die before lv_deinit(). It backs a
    // component registered once at boot, so it outlives a printer switch.
    StaticSubjectRegistry::instance().register_deinit("AfcFaultPath", afc_fault_path_deinit,
                                                      StaticSubjectRegistry::Lifetime::Process);
}

} // namespace

void afc_fault_path_register() {
    ensure_subject();

    // Registered here rather than alongside the modals that embed it:
    // ams_loading_error_modal.xml (lazily registered by AmsPanel),
    // action_prompt_modal.xml and recovery_fault_modal.xml reference
    // <afc_fault_path/>, so it has to exist before any of them is parsed.
    const std::string path =
        "A:" + LayoutManager::instance().resolve_xml_path("components/afc_fault_path.xml");
    if (lv_xml_register_component_from_file(path.c_str()) != LV_RESULT_OK) {
        spdlog::error("[AfcFaultPath] Failed to register: {}", path);
    }

    spdlog::trace("[AfcFaultPath] Subject and component registered");
}

std::optional<PathSegment> afc_fault_path_publish(std::string_view message) {
    const auto segment = helix::afc::afc_fault_position(message);
    ensure_subject();
    lv_subject_set_int(&g_fault_segment, segment ? static_cast<int>(*segment) : 0);
    if (segment) {
        spdlog::debug("[AfcFaultPath] Fault position: {}", path_segment_to_string(*segment));
    }
    return segment;
}

int afc_fault_path_current() {
    return g_initialized ? lv_subject_get_int(&g_fault_segment) : 0;
}

void afc_fault_path_restore(int published, int previous) {
    if (g_initialized && lv_subject_get_int(&g_fault_segment) == published) {
        lv_subject_set_int(&g_fault_segment, previous);
    }
}

std::string afc_fault_path_apply(const std::string& message) {
    if (!afc_fault_path_publish(message)) {
        return message;
    }
    return helix::afc::afc_strip_position_diagram(message);
}

void AfcFaultPathHold::hold() {
    if (!held_) {
        before_ = afc_fault_path_current();
        held_ = true;
    }
}

std::optional<PathSegment> AfcFaultPathHold::publish(std::string_view message) {
    hold();
    const auto segment = afc_fault_path_publish(message);
    published_ = segment ? static_cast<int>(*segment) : 0;
    return segment;
}

std::string AfcFaultPathHold::apply(const std::string& message) {
    hold();
    std::string shown = afc_fault_path_apply(message);
    published_ = afc_fault_path_current();
    return shown;
}

void AfcFaultPathHold::release() {
    if (held_) {
        afc_fault_path_restore(published_, before_);
        held_ = false;
    }
}

} // namespace helix::ui
