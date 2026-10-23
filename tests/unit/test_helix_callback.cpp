// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix::Callback<Sig> replaces std::function on the UpdateQueue and observer
// APIs. These tests pin the storage path each kind of callable takes and the
// ownership rule every path must keep: a stored callable is destroyed exactly
// once, whether it is invoked, moved, overwritten or dropped unrun.

#include "helix_callback.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::Callback;
using helix::callback_storage;
using helix::CallbackStorage;

namespace {

/// Counts destructor runs of every live or moved-from instance.
struct DtorCounter {
    int* count;
    explicit DtorCounter(int* c) : count(c) {}
    DtorCounter(DtorCounter&& o) noexcept : count(o.count) {
        o.count = nullptr;
    }
    DtorCounter(const DtorCounter&) = delete;
    ~DtorCounter() {
        if (count != nullptr) {
            ++*count;
        }
    }
};

/// Reports the address the callable runs at, which tells inline storage
/// (moves with the Callback) from heap storage (stays put).
struct AddressReporter {
    const void** seen;
    void operator()() const {
        *seen = this;
    }
};

struct BigAddressReporter {
    const void** seen;
    std::array<char, helix::callback_detail::kInlineSize + 1> pad{};
    void operator()() const {
        *seen = this;
    }
};

int add_one(int v) {
    return v + 1;
}

} // namespace

TEST_CASE("Callback: invokes with arguments and returns the result", "[helix_callback]") {
    Callback<int(int, int)> cb = [](int a, int b) { return a * 10 + b; };
    REQUIRE(cb);
    REQUIRE(cb(4, 2) == 42);

    Callback<int(int)> fp = &add_one;
    REQUIRE(fp(1) == 2);
}

TEST_CASE("Callback: storage path follows the callable's shape", "[helix_callback]") {
    int x = 0;
    auto trivial = [&x, y = 3]() { x += y; };
    auto with_string = [s = std::string("payload")]() { (void)s; };
    auto with_token = [t = std::make_shared<int>(1)]() { (void)t; };
    auto oversized = [pad = std::array<char, helix::callback_detail::kInlineSize + 1>{}]() {
        (void)pad;
    };

    STATIC_REQUIRE(callback_storage<decltype(trivial)>() == CallbackStorage::Trivial);
    STATIC_REQUIRE(callback_storage<decltype(with_string)>() == CallbackStorage::Inline);
    STATIC_REQUIRE(callback_storage<decltype(with_token)>() == CallbackStorage::Inline);
    STATIC_REQUIRE(callback_storage<decltype(oversized)>() == CallbackStorage::Heap);
}

TEST_CASE("Callback: a trivially copyable callable moves with its state", "[helix_callback]") {
    int total = 0;
    Callback<void()> a = [&total, step = 5]() { total += step; };
    a();
    Callback<void()> b = std::move(a);
    REQUIRE_FALSE(a);
    b();
    REQUIRE(total == 10);
}

TEST_CASE("Callback: inline storage relocates, heap storage stays put", "[helix_callback]") {
    const void* seen = nullptr;

    SECTION("inline") {
        Callback<void()> a = AddressReporter{&seen};
        a();
        const void* before = seen;
        Callback<void()> b = std::move(a);
        b();
        REQUIRE(seen != before);
    }

    SECTION("heap") {
        Callback<void()> a = BigAddressReporter{&seen};
        a();
        const void* before = seen;
        Callback<void()> b = std::move(a);
        b();
        REQUIRE(seen == before);
    }
}

TEST_CASE("Callback: a non-trivial callable is destroyed exactly once", "[helix_callback]") {
    int dtors = 0;

    SECTION("invoked then destroyed") {
        {
            Callback<void()> cb = [c = DtorCounter(&dtors)]() {};
            cb();
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }

    SECTION("moved twice") {
        {
            Callback<void()> a = [c = DtorCounter(&dtors)]() {};
            Callback<void()> b = std::move(a);
            Callback<void()> c;
            c = std::move(b);
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }

    SECTION("overwritten by assignment") {
        Callback<void()> cb = [c = DtorCounter(&dtors)]() {};
        cb = [] {};
        REQUIRE(dtors == 1);
        cb = nullptr;
        REQUIRE_FALSE(cb);
        REQUIRE(dtors == 1);
    }

    SECTION("oversized, on the heap") {
        {
            Callback<void()> a = [c = DtorCounter(&dtors),
                                  pad = std::array<char, helix::callback_detail::kInlineSize>{}]() {
                (void)pad;
            };
            Callback<void()> b = std::move(a);
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }
}

TEST_CASE("Callback: holds a move-only capture", "[helix_callback]") {
    auto owned = std::make_unique<int>(7);
    int seen = 0;
    Callback<void()> cb = [p = std::move(owned), &seen]() { seen = *p; };
    Callback<void()> moved = std::move(cb);
    moved();
    REQUIRE(seen == 7);
}

TEST_CASE("Callback: a const call keeps a mutable callable's state", "[helix_callback]") {
    const Callback<int()> counter = [n = 0]() mutable { return ++n; };
    REQUIRE(counter() == 1);
    REQUIRE(counter() == 2);
}

TEST_CASE("Callback: empty states", "[helix_callback]") {
    REQUIRE_FALSE(Callback<void()>{});
    REQUIRE_FALSE(Callback<void()>{nullptr});

    void (*null_fp)() = nullptr;
    REQUIRE_FALSE(Callback<void()>{null_fp});
    REQUIRE_FALSE(Callback<void()>{std::function<void()>{}});

    // Desktop builds throw like std::function; the firmware aborts.
    Callback<void()> empty;
    REQUIRE_THROWS_AS(empty(), std::bad_function_call);
}
