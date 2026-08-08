#pragma once
#include <cstdint>
#include <atomic>
#include <array>
#include <cstdlib>

// ============================================================================
// G1.2 — env-gated MIDI/voice/looper event trace.
//
// A debugging lens for "notes pile up / get stuck" bugs (looper #138, poly #141):
// it records, in time order, every note reaching the engine, every voice alloc /
// steal / stop, and every looper record / emit / wrap — so the exact event that
// starts an extra voice is NAMED, not guessed.
//
// OFF by default; enabled by `VASYNTH_MIDI_TRACE=1` in the environment. When off,
// every emit() is a single relaxed atomic-bool load + return (zero cost, RT-safe).
// When on, the audio thread only PUSHES PODs into a lock-free SPSC ring — no alloc,
// lock or IO on the audio thread. The owner (the processor, message thread) drains
// the ring to a file (see MidiTraceWriter in PluginProcessor). JUCE-free and
// header-only so the DSP headers (SynthEngine/Looper) and their unit tests can
// instrument without any extra link dependency.
//
// Field meanings per kind (a,b,c,d) are documented in docs/midi-trace.md.
// ============================================================================

namespace mtrace
{
    enum class Ev : std::uint8_t
    {
        BlockStart,     // a=numSamples                          (context: a new audio block)
        NoteOnLive,     // a=note b=vel127 c=part d=voiceIdx     (live/played voice allocated)
        NoteOnGen,      // a=note b=vel127 c=part d=voiceIdx     (generator arp/seq/looper voice allocated)
        NoteRetrig,     // a=note b=vel127 c=part d=voiceIdx     (same note re-hit on its existing voice)
        NoteOff,        // a=note b=part   c=voiceIdx            (voice released)
        VoiceSteal,     // a=note b=part   c=stealIdx d=victimNote (pool full: victim voice stolen)
        CC,             // a=ccNum b=value c=part
        Panic,          // (all-notes-off requested)
        AllNotesOff,    // (engine flushed every voice)
        LoopRec,        // a=part b=note c=vel127 d=on?1:0       (event recorded into a lane)
        LoopEmit,       // a=part b=note c=on?1:0 d=t            (armed event played back this block)
        LoopWrap        // a=part b=eventCount                    (lane wrapped -> its events (re)armed)
    };

    struct Event
    {
        std::uint64_t block;    // audio block index
        std::uint32_t frame;    // sample offset within the block (context)
        Ev            kind;
        std::int16_t  a, b, c, d;
    };

    // Lock-free SPSC ring + enable flag. One producer (audio thread) pushes; one
    // consumer (message thread) drains. No thread, no IO here — the owner writes.
    class Tracer
    {
    public:
        static constexpr int kCap = 1 << 15;               // 32768 events (power of two)

        Tracer()
        {
            const char* e = std::getenv ("VASYNTH_MIDI_TRACE");
            on_.store (e != nullptr && e[0] == '1', std::memory_order_relaxed);
        }

        bool on() const noexcept { return on_.load (std::memory_order_relaxed); }
        void setEnabled (bool b) noexcept { on_.store (b, std::memory_order_relaxed); }   // test hook

        // Producer context (audio thread only): the block/frame stamped on subsequent pushes.
        void setBlock (std::uint64_t block, std::uint32_t frame) noexcept { curBlock_ = block; curFrame_ = frame; }

        void push (Ev kind, int a, int b, int c, int d) noexcept
        {
            if (! on_.load (std::memory_order_relaxed)) return;
            const auto w  = wr_.load (std::memory_order_relaxed);
            const auto nx = (w + 1) & kMask;
            if (nx == rd_.load (std::memory_order_acquire)) { dropped_.fetch_add (1, std::memory_order_relaxed); return; }
            buf_[w] = { curBlock_, curFrame_, kind,
                        (std::int16_t) a, (std::int16_t) b, (std::int16_t) c, (std::int16_t) d };
            wr_.store (nx, std::memory_order_release);
        }

        // Consumer (message thread): pop every queued event through sink(const Event&).
        template <typename Sink>
        void drain (Sink&& sink) noexcept
        {
            auto r = rd_.load (std::memory_order_relaxed);
            while (r != wr_.load (std::memory_order_acquire))
            {
                sink (buf_[r]);
                r = (r + 1) & kMask;
                rd_.store (r, std::memory_order_release);
            }
        }

        std::uint64_t dropped() const noexcept { return dropped_.load (std::memory_order_relaxed); }
        bool empty() const noexcept { return rd_.load (std::memory_order_acquire) == wr_.load (std::memory_order_acquire); }

    private:
        static constexpr std::uint32_t kMask = (std::uint32_t) kCap - 1;
        std::array<Event, (std::size_t) kCap> buf_ {};
        std::atomic<std::uint32_t> wr_ { 0 }, rd_ { 0 };
        std::atomic<std::uint64_t> dropped_ { 0 };
        std::atomic<bool> on_ { false };
        std::uint64_t curBlock_ { 0 };
        std::uint32_t curFrame_ { 0 };
    };

    // One process-wide instance (lazy). A debug facility; a single trace across plugin instances is fine.
    inline Tracer& tracer() { static Tracer t; return t; }

    // Instrumentation-site helpers (cheap no-ops when disabled).
    inline bool on() noexcept { return tracer().on(); }
    inline void setBlock (std::uint64_t block, std::uint32_t frame = 0) noexcept
    { if (tracer().on()) tracer().setBlock (block, frame); }
    inline void emit (Ev k, int a = 0, int b = 0, int c = 0, int d = 0) noexcept { tracer().push (k, a, b, c, d); }
}
