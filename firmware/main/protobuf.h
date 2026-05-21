// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 17 — RAII C++ wrapper around generated nanopb message structs.
//
// nanopb's "static allocation" mode (FT_STATIC) bakes worst-case array
// sizes into the generated struct, which forced the Stage 15/16 caps in
// `proto/widgets.options` (Screen.widgets = 32, etc.) and inflated the
// decoded `touchy_Screen` to ~70 KB on the stack/.bss before we even
// touched it. Stage 17 switches the variable-length fields to
// `FT_POINTER` (heap allocation via `pb_realloc`); this wrapper hides
// the matching `pb_release()` bookkeeping behind a normal C++ value
// type.
//
// Usage:
//
//   PbMessage<touchy_Screen> msg(touchy_Screen_fields);
//   if (!msg.decode(buf, len)) { ... }
//   for (pb_size_t i = 0; i < msg->widgets_count; i++) { ... }
//   // pb_release() called automatically when `msg` goes out of scope.
//
// The wrapper owns the underlying struct by value (so accessors return
// references, not pointers-into-heap). Move semantics are supported so
// instances can be enqueued via std::unique_ptr without an extra
// allocation hop. Copying is disabled — every clone would need to
// re-encode + re-decode, which is rarely what callers want by default;
// use the explicit `clone()` helper instead.

#pragma once

#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <cstddef>
#include <cstdint>
#include <utility>

template <typename T>
class PbMessage {
public:
    // `fields` is the `xxx_fields` descriptor emitted by nanopb for `T`.
    // We accept it at runtime (rather than as a template non-type
    // argument) to keep the call site terse and avoid one template
    // instantiation per descriptor pointer.
    explicit PbMessage(const pb_msgdesc_t *fields) noexcept
        : fields_(fields), msg_{} {}

    ~PbMessage() noexcept { release(); }

    PbMessage(const PbMessage &)            = delete;
    PbMessage &operator=(const PbMessage &) = delete;

    PbMessage(PbMessage &&other) noexcept
        : fields_(other.fields_), msg_(other.msg_)
    {
        other.msg_ = T{};
    }
    PbMessage &operator=(PbMessage &&other) noexcept
    {
        if (this != &other) {
            release();
            fields_    = other.fields_;
            msg_       = other.msg_;
            other.msg_ = T{};
        }
        return *this;
    }

    // Reset to an all-zero default value, freeing any previously
    // decoded heap allocations.
    void clear() noexcept
    {
        release();
        msg_ = T{};
    }

    // Decode `len` bytes into the held struct. Any previously decoded
    // contents are released first so repeated decodes don't leak.
    bool decode(const uint8_t *data, std::size_t len) noexcept
    {
        clear();
        pb_istream_t s = pb_istream_from_buffer(data, len);
        if (pb_decode(&s, fields_, &msg_)) return true;
        // pb_decode partially populates on failure; release the leftovers.
        release();
        msg_ = T{};
        return false;
    }

    // Encode into `out` (caller-provided buffer). On success, writes the
    // payload length to `*written` and returns true.
    bool encode(uint8_t *out, std::size_t cap, std::size_t *written) const noexcept
    {
        pb_ostream_t s = pb_ostream_from_buffer(out, cap);
        if (!pb_encode(&s, fields_, &msg_)) return false;
        if (written) *written = s.bytes_written;
        return true;
    }

    // Deep-copy via re-encode/re-decode. Only path that nanopb gives us
    // to duplicate a heap-owning message without writing a per-type
    // walker. Caller supplies the scratch buffer to avoid an extra
    // heap allocation on the hot enqueue path.
    bool clone_into(PbMessage<T> &dst,
                    uint8_t *scratch, std::size_t scratch_cap) const noexcept
    {
        std::size_t n = 0;
        if (!encode(scratch, scratch_cap, &n)) return false;
        return dst.decode(scratch, n);
    }

    // Pointer-like access to the underlying struct.
    T       *operator->() noexcept       { return &msg_; }
    const T *operator->() const noexcept { return &msg_; }
    T       &operator*()  noexcept       { return msg_; }
    const T &operator*()  const noexcept { return msg_; }
    T       *get()        noexcept       { return &msg_; }
    const T *get()        const noexcept { return &msg_; }

    const pb_msgdesc_t *fields() const noexcept { return fields_; }

private:
    void release() noexcept
    {
        if (fields_) pb_release(fields_, &msg_);
    }

    const pb_msgdesc_t *fields_;
    T                   msg_;
};
