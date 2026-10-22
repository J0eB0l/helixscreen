// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The edit list a print-start rewrite applies, in the form the HelixPrint
// plugin's server.helix.print_edited takes, so the plugin can apply it beside
// the file instead of the screen downloading, rewriting and uploading it.
//
// The grammar has two implementations: GCodeFileModifier::apply_streaming and
// GcodeToolRemapper::apply_to_file here, moonraker-plugin/helix_edits.py
// there. tests/fixtures/gcode_edits/ holds cases both suites apply, byte for
// byte.

#include "gcode_file_modifier.h"
#include "helix_plugin_features.h"

#include <map>
#include <optional>
#include <vector>

#include "hv/json.hpp"

namespace helix::gcode {

/// The plugin feature that serves print_edited, and the contract this app writes.
inline constexpr const char* kServerEditsFeature = "server_edits";
inline constexpr int kServerEditsMinVersion = 1;

/// One rewrite of a file: positional edits, or a tool remap (logical tool ->
/// physical head) over every line. A list carries one or the other.
struct EditList {
    std::vector<Modification> modifications;
    std::map<int, int> remap;
};

/// @p list as print_edited's `edits` array, or nullopt when it cannot travel
/// and the rewrite stays on the screen: an empty list, one carrying both
/// edits and a remap, a range on an edit that has none, or text that is not
/// UTF-8 (JSON would substitute its bytes, and the copy would print them).
std::optional<nlohmann::json> to_server_edits(const EditList& list);

/// The list a print_edited `edits` array names, or nullopt for one the
/// plugin refuses as malformed.
std::optional<EditList> from_server_edits(const nlohmann::json& edits);

/// Where a print-start rewrite runs.
enum class EditTransport {
    Plugin,          ///< print_edited: the plugin rewrites beside the file
    DownloadRewrite, ///< the screen downloads, rewrites and uploads the copy
};

/// The plugin rewrites when its feature is ready at kServerEditsMinVersion and
/// the list can travel (@p expressible). An archive plate always rewrites on
/// the screen: the plate has to be extracted from the archive first.
EditTransport choose_edit_transport(PluginFeatureState server_edits, bool archive_plate,
                                    bool expressible);

} // namespace helix::gcode
