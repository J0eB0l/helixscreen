// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ipp_printer_http.cpp
 * @brief The IPP job POST against a real loopback socket server.
 *
 * Some printers (Canon PIXMA) answer a POST with an unsolicited interim
 * `HTTP/1.1 100 Continue` before the final response, with or without an
 * `Expect:` header. The client has to read past it to the final response
 * (RFC 9110 15.2). The server here is a raw socket so the test controls
 * exactly which bytes arrive in which write (prestonbrown/helixscreen#1822).
 */

#include "hv/HttpMessage.h"
#include "ipp_printer.h"
#include "ipp_protocol.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

/// A minimal IPP response body: version 2.0, successful-ok, request-id 1, end tag.
std::string ipp_ok_body() {
    const unsigned char bytes[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::string final_ok_response() {
    const auto body = ipp_ok_body();
    return "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// One-connection HTTP server on 127.0.0.1 that reads the whole request and then
/// writes each scripted chunk, pausing between chunks so each lands in its own recv.
class ScriptedServer {
  public:
    explicit ScriptedServer(std::vector<std::string> chunks) : chunks_(std::move(chunks)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 1);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~ScriptedServer() {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable())
            thread_.join();
    }

    uint16_t port() const {
        return port_;
    }
    size_t body_bytes_received() const {
        return body_bytes_.load();
    }

  private:
    void serve() {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
            return;
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string request;
        char buf[4096];
        size_t header_end = std::string::npos;
        size_t content_length = 0;
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            request.append(buf, static_cast<size_t>(n));
            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto pos = request.find("Content-Length: ");
                    if (pos != std::string::npos && pos < header_end)
                        content_length = std::stoul(request.substr(pos + 16));
                }
            }
            if (header_end != std::string::npos &&
                request.size() >= header_end + 4 + content_length)
                break;
        }
        if (header_end != std::string::npos)
            body_bytes_ = request.size() - header_end - 4;

        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (i > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            ::send(fd, chunks_[i].data(), chunks_[i].size(), MSG_NOSIGNAL);
        }
        // Keep the connection open until the client hangs up, as a printer would.
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {
        }
        ::close(fd);
    }

    std::vector<std::string> chunks_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<size_t> body_bytes_{0};
    std::thread thread_;
};

std::vector<uint8_t> job_body() {
    return std::vector<uint8_t>(20000, 0x5A);
}

void require_ipp_ok(const std::shared_ptr<HttpResponse>& resp) {
    REQUIRE(resp != nullptr);
    REQUIRE(static_cast<int>(resp->status_code) == 200);
    auto ipp_resp =
        ipp::parse_response(reinterpret_cast<const uint8_t*>(resp->body.data()), resp->body.size());
    REQUIRE(ipp_resp.is_success());
}

} // namespace

TEST_CASE("IPP POST reads past an interim 100 Continue to the final response",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue, final_ok_response()});
    auto body = job_body();

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", body, 10);

    require_ipp_ok(resp);
    REQUIRE(server.body_bytes_received() == body.size());
}

TEST_CASE("IPP POST handles 100 Continue and the final response in one write",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({std::string(kContinue) + final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST reads past several interim responses", "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue, "HTTP/1.1 102 Processing\r\n\r\n", final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST returns a final response that has no interim", "[label][ipp][ipp_http]") {
    ScriptedServer server({final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST returns nullptr when only an interim response arrives",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 2);

    REQUIRE(resp == nullptr);
}
