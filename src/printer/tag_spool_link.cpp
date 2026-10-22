// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tag_spool_link.h"

namespace helix::ams {

std::optional<int> TagSpoolLinkTracker::observe(int slot_index, const TagSpoolSighting& sighting) {
    auto [it, first_sight] = lanes_.try_emplace(slot_index);
    Lane& lane = it->second;
    if (!sighting.occupied) {
        // The spool left, so whatever is seated next is a new insert.
        lane = Lane{};
        return std::nullopt;
    }
    if (!sighting.tag_spool_id) {
        // A seated spool whose tag names no spool, or has not been read yet,
        // says nothing about which spool it is.
        if (first_sight) {
            lanes_.erase(it);
        }
        return std::nullopt;
    }
    const int id = *sighting.tag_spool_id;
    if (first_sight) {
        lane.seen = id;
        if (sighting.lane_spoolman_id == 0) {
            lane.want = id;
        }
    } else if (lane.seen != id) {
        lane = Lane{id, id, false};
    }
    if (lane.want && *lane.want == sighting.lane_spoolman_id) {
        lane.want.reset();
        lane.in_flight = false;
    }
    if (!lane.want || lane.in_flight) {
        return std::nullopt;
    }
    return lane.want;
}

void TagSpoolLinkTracker::begin_lookup(int slot_index, int spool_id) {
    auto it = lanes_.find(slot_index);
    if (it != lanes_.end() && it->second.want == spool_id) {
        it->second.in_flight = true;
    }
}

bool TagSpoolLinkTracker::wants(int slot_index, int spool_id) const {
    auto it = lanes_.find(slot_index);
    return it != lanes_.end() && it->second.want == spool_id;
}

void TagSpoolLinkTracker::settle(int slot_index, int spool_id) {
    auto it = lanes_.find(slot_index);
    if (it != lanes_.end() && it->second.want == spool_id) {
        it->second.want.reset();
        it->second.in_flight = false;
    }
}

} // namespace helix::ams
