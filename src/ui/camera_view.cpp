// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_view.h"

#if HELIX_HAS_CAMERA

#include "display_manager.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix {

CameraView::CameraView() = default;

CameraView::~CameraView() {
    stop({});
}

bool CameraView::start(const std::string& source, const nlohmann::json& transform_config,
                       FrameFn on_frame, StatusFn on_status) {
    if (is_running()) {
        spdlog::debug("[CameraView] start: already running, skipping");
        return true;
    }

    auto feed = CameraStream::resolve_from_printer(source);
    if (!feed) {
        spdlog::debug("[CameraView] start: no feed resolves for '{}' yet", source);
        return false;
    }
    feed_ = *feed;
    stream_ = std::make_unique<CameraStream>();
    apply_transform(transform_config);

    // Decode at display resolution instead of full camera resolution, which
    // spares LVGL a software scale on every draw.
    if (auto* disp = lv_display_get_default()) {
        stream_->set_target_size(lv_display_get_horizontal_resolution(disp),
                                 lv_display_get_vertical_resolution(disp));
    }

    target_fps_ = feed_.target_fps > 0 ? feed_.target_fps : 15;
    stream_->set_max_fps(target_fps_);

    // The callbacks capture copies of the owner's functions, not `this`: a
    // stream whose thread outlives stop() (see was_detached()) must never reach
    // a destroyed view. The token drops anything queued after stop().
    auto token = stream_lifetime_.token();
    stream_->start(
        feed_.stream_url, feed_.snapshot_url,
        [token, on_frame = std::move(on_frame)](lv_draw_buf_t* frame) {
            token.defer("CameraView::frame", [on_frame, frame]() {
                if (on_frame) {
                    on_frame(frame);
                }
            });
        },
        [token, on_status = std::move(on_status)](const char* msg) {
            std::string status(msg);
            token.defer("CameraView::status", [on_status, status]() {
                if (on_status) {
                    on_status(status);
                }
            });
        });

    spdlog::info("[CameraView] Stream started (camera='{}', stream={}, snapshot={})",
                 feed_.name.empty() ? "<auto>" : feed_.name, feed_.stream_url, feed_.snapshot_url);
    return true;
}

void CameraView::stop(std::initializer_list<lv_obj_t*> images) {
    if (!stream_) {
        return;
    }

    // Queued frame deferrals become no-ops before their buffers are freed.
    stream_lifetime_.invalidate();

    // stop() frees the draw buffers the images may still reference, and the
    // render thread can still be blending from one it picked up before the
    // source was cleared (#749).
    if (lv_is_initialized()) {
        for (lv_obj_t* image : images) {
            if (image) {
                lv_image_set_src(image, nullptr);
            }
        }
        lv_draw_wait_for_finish();
    }

    stream_->stop();

    if (stream_->was_detached()) {
        // The stream thread still runs with a captured pointer to the stream,
        // so destroying it would be a use-after-free (#624). It winds down on
        // its own once the HTTP request completes.
        spdlog::warn("[CameraView] Stream thread was detached, leaking CameraStream to avoid UAF");
        (void)stream_.release();
    } else {
        stream_.reset();
    }
    spdlog::debug("[CameraView] Stream stopped");
}

void CameraView::set_max_fps(int fps) {
    if (stream_) {
        stream_->set_max_fps(fps);
    }
}

void CameraView::apply_transform(const nlohmann::json& transform_config) {
    if (!stream_) {
        return;
    }
    const auto t = CameraStream::transform_from_config(transform_config, feed_);
    stream_->set_rotation(t.rotation);
    stream_->set_flip(t.flip_h, t.flip_v);
    spdlog::debug("[CameraView] Transform: rotation={}, flip_h={}, flip_v={}",
                  static_cast<int>(t.rotation), t.flip_h, t.flip_v);
}

void CameraView::on_display_sleep(std::function<void(bool sleeping)> on_change) {
    if (sleep_handler_) {
        *sleep_handler_ = std::move(on_change);
        return;
    }
    auto* dm = DisplayManager::instance();
    if (!dm) {
        return;
    }
    sleep_handler_ = std::make_shared<std::function<void(bool)>>(std::move(on_change));
    // DisplayManager keeps every callback for the life of the process, so the
    // registration holds a weak handle that dies with the view, never the view.
    std::weak_ptr<std::function<void(bool)>> handler = sleep_handler_;
    dm->register_sleep_callback([handler](bool sleeping) {
        if (auto fn = handler.lock(); fn && *fn) {
            (*fn)(sleeping);
        }
    });
}

} // namespace helix

#endif // HELIX_HAS_CAMERA
