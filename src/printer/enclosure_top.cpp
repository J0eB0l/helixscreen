// SPDX-License-Identifier: GPL-3.0-or-later

#include "enclosure_top.h"

#include "printer_discovery.h"

#include <algorithm>
#include <vector>

namespace helix::enclosure_top {
namespace {

/// One firmware that reports a removable enclosure top.
struct Provider {
    const char* name;
    /// Status object carrying the reading. Its presence in the object list is
    /// also what identifies the firmware.
    const char* status_object;
    /// Boolean field: true while the top is fitted.
    const char* fitted_field;
};

// The Snapmaker U1's top module carries an air purifier that the main board
// powers through the top's connector, so the board sensing that power is the
// top being on the printer.
const std::vector<Provider>& providers() {
    static const std::vector<Provider> kProviders = {
        {"Snapmaker U1", "purifier", "power_detected"},
    };
    return kProviders;
}

const Provider* provider_for(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    for (const auto& p : providers()) {
        if (std::find(objects.begin(), objects.end(), p.status_object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool firmware_reports_enclosure_top(const PrinterDiscovery& hw) {
    return provider_for(hw) != nullptr;
}

nlohmann::json required_status_objects(const PrinterDiscovery& hw) {
    nlohmann::json objects = nlohmann::json::object();
    if (const Provider* p = provider_for(hw)) {
        // Narrowed to the one field: the same object reports fan speeds that
        // would otherwise notify on every tick.
        objects[p->status_object] = nlohmann::json::array({p->fitted_field});
    }
    return objects;
}

std::optional<bool> fitted_from_status(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    for (const auto& p : providers()) {
        auto obj = status.find(p.status_object);
        if (obj == status.end() || !obj->is_object()) {
            continue;
        }
        auto field = obj->find(p.fitted_field);
        if (field == obj->end() || !field->is_boolean()) {
            continue;
        }
        return field->get<bool>();
    }
    return std::nullopt;
}

} // namespace helix::enclosure_top
