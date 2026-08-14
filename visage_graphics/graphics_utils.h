/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <ctime>

#include <cstdlib>

#include <cstdio>

#include "visage_utils/defines.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bgfx {
  struct VertexLayout;
  struct TextureHandle;
  struct ShaderHandle;
  struct ProgramHandle;
  struct UniformHandle;
  struct IndexBufferHandle;
  struct FrameBufferHandle;
  struct TransientIndexBuffer;
  struct TransientVertexBuffer;
}

namespace visage {
  enum class BlendMode {
    Opaque,
    Composite,
    Alpha,
    Add,
    Sub,
    Mult,
    MaskAdd,
    MaskRemove,
  };

  static constexpr float kHdrColorRange = 4.0f;
  static constexpr float kHdrColorMultiplier = 1.0f / kHdrColorRange;
  static constexpr int kVerticesPerQuad = 4;

  /// A BUILD THAT TRACES WITHOUT BEING ASKED, and the reason it is a compile flag rather than one
  /// more environment variable.
  ///
  /// Every trace in this file is opt-in through the environment, which is right for a shipping build
  /// and wrong for the one instrument that matters when a fault will not reproduce under a harness:
  /// a build handed to the person whose hands DO reproduce it. An environment variable has to survive
  /// a launcher, a sandbox and a habit, and any of the three can drop it without saying so — leaving
  /// a session that looks instrumented, is not, and whose empty log then reads as evidence. Compiled
  /// in, the trace cannot be forgotten on the command line and cannot be turned off by accident.
  ///
  /// OFF BY DEFAULT, so an ordinary build of this tree is byte-for-byte what it was. Only
  /// `-DFEATHERS_DIAGNOSTIC=ON` at configure time turns it on.
  constexpr bool diagnosticBuild() {
#ifdef FEATHERS_DIAGNOSTIC
    return true;
#else
    return false;
#endif
  }

  /// CLOCK_REALTIME, in milliseconds, so a line can be lined up against a screenshot's mtime or a
  /// capture named with `date +%s%N`. An earlier version used CLOCK_MONOTONIC — which counts from
  /// boot — and the two differ by five orders of magnitude, so every correlation would have been
  /// confident nonsense.
  ///
  /// POSIX ONLY, AND THIS HEADER IS INCLUDED WIDELY. clock_gettime and <ctime>'s CLOCK_REALTIME are
  /// not available under MSVC, and this fork's CMake carries an MSVC branch — so a Windows build of
  /// it would fail here rather than at any one call site. The C11 spelling that compiles everywhere
  /// is `timespec_get(&ts, TIME_UTC)`, which is what this becomes if the traces are ever wanted off
  /// POSIX. Left as it is deliberately: they have one purpose, on one machine, chasing one bug.
  inline long long traceMilliseconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  }

  /// The atlas trace's gate, hoisted so the glyph trace below shares it rather than reading the
  /// environment a second time under a second static.
  inline bool atlasTraceEnabled() {
    static const bool enabled = diagnosticBuild() || std::getenv("VISAGE_TRACE_ATLAS") != nullptr;
    return enabled;
  }

  /// OPT-IN ATLAS TRACE, off unless VISAGE_TRACE_ATLAS is set in the environment or this is a
  /// diagnostic build.
  ///
  /// An atlas resize reallocates a texture and repacks everything in it, so a diagnostic beside one
  /// costs nothing that matters — and the gate is a function-local static read after the first call,
  /// so a build with the variable unset pays one predictable branch on an already-expensive path.
  /// That is what lets this live in a SHIPPING build rather than in a special one: the venue where a
  /// rare rendering fault actually appears is somebody's ordinary session, and a fault you have to
  /// reproduce in a debug build first is a fault you mostly do not catch.
  ///
  /// NOT RATE-BOUNDED, unlike the glyph line below, and that asymmetry is the point of the pair. A
  /// repack is the event that moves already-submitted draws, so every single one has to be readable
  /// against the moment a screenshot was taken; there are tens of them in a session, not thousands.
  ///
  /// THE SIZE AND THE RECT COUNT ARE WHAT MAKE THE WIDTH READABLE, and their absence cost a session's
  /// log analysis real time. Every font SIZE owns a SEPARATE atlas, so a bare width cannot say which
  /// one moved. And the width alone genuinely misleads, because `PackedAtlasMap::pack` has two
  /// branches: with exactly ONE rect it sizes the atlas to that glyph plus a pixel of padding — which
  /// is where absurd-looking widths like 8, 9 and 13 come from — and with two or more it starts at
  /// kDefaultWidth, 64. So a (small, 64) PAIR on one size is not an atlas growing under pressure. It
  /// is a PackedFont being BORN: its first glyph, then its second. The count says which branch ran so
  /// that nobody has to infer it from the number.
  inline void traceAtlasResize(const char* which, int size, int newWidth, int numRects) {
    if (!atlasTraceEnabled()) {
      return;
    }
    std::fprintf(stderr, "[VISAGE-ATLAS] %s size %d resize -> %d (%d rects) at %lld\n", which, size,
                 newWidth, numRects, traceMilliseconds());
    std::fflush(stderr);
  }

  /// A FONT SIZE USED FOR THE FIRST TIME — the event a (small, 64) pair of resizes is only the shadow
  /// of.
  ///
  /// PackedFonts are cached per typeface and PIXEL SIZE, so a size never drawn before mints an entire
  /// new atlas: an empty packer, a resize for glyph one, another for glyph two. Reconstructing that
  /// from resize widths is guesswork, and guesswork about which font moved is exactly what stalls an
  /// analysis. This states it, with the size — so "a new font size appeared mid-drag" becomes a line
  /// rather than an inference, and if one does appear mid-gesture then whatever computes a font size
  /// from a live value is worth finding.
  ///
  /// Unbounded: a session holds a few dozen sizes at the very most.
  inline void traceFontCreated(int size) {
    if (!atlasTraceEnabled()) {
      return;
    }
    std::fprintf(stderr, "[VISAGE-ATLAS] FONT size %d CREATED at %lld\n", size, traceMilliseconds());
    std::fflush(stderr);
  }

  /// EVERY GLYPH THE FONT ATLAS LEARNS — the churn a repack line only reports the END of.
  ///
  /// A repack is the event that invalidates submitted draws; a glyph ADD is what walks the atlas
  /// towards one. Having both says whether a photographed fault landed on the frame that repacked or
  /// on one of the hundreds that merely grew — and it says what was being TYPED at the time, which is
  /// the differentiator this particular hunt turns on. A readout counting "-3.66" through "-3.71"
  /// hands the atlas characters it has never packed, once per gesture, and a parameter storm driven
  /// through the automation road hands it none.
  ///
  /// UNBOUNDED, and it was bounded until a real session proved that wrong. First-200-then-every-50th
  /// looked prudent and cost the analysis its middle: glyph #350 was logged at startup and #400
  /// twelve seconds later, so adds #351 through #399 — every one of them inside the gesture window
  /// being investigated — had no timestamps at all. A thinned line cannot be correlated with anything,
  /// which for this line is the entire purpose.
  ///
  /// MEASURED BEFORE BEING UNBOUNDED, rather than assumed affordable: a startup packs about 200 and a
  /// long session a few hundred more. That is a handful of kilobytes. The cap below is a disk-fill
  /// guard set two orders of magnitude above anything observed, not a sampling rate — if it ever
  /// fires, that is itself the finding, and it says so on the way out.
  inline void traceGlyphPacked(char32_t character, int font_size, int atlas_width) {
    if (!atlasTraceEnabled()) {
      return;
    }

    static constexpr int kRunawayGuard = 50000;
    static std::atomic<int> packed { 0 };
    const int ordinal = packed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal > kRunawayGuard) {
      return;
    }
    if (ordinal == kRunawayGuard) {
      std::fprintf(stderr,
                   "[VISAGE-ATLAS] glyph runaway: %d packed, far past anything measured - "
                   "further glyph lines suppressed\n",
                   kRunawayGuard);
      std::fflush(stderr);
      return;
    }

    // PRINTABLE ASCII GETS ITS CHARACTER AND EVERYTHING ELSE A DOT, with the code point printed
    // either way. Reading back the string a gesture typed is most of the value, and a char32_t handed
    // to %c would print something else entirely for anything above the ASCII range; a code point on
    // its own is correct but has to be decoded by hand for the common case, which is a letter.
    const bool printable = character >= 0x20 && character < 0x7f;
    std::fprintf(stderr, "[VISAGE-ATLAS] glyph #%d U+%04X '%c' size %d atlas %d at %lld\n", ordinal,
                 static_cast<unsigned>(character), printable ? static_cast<char>(character) : '.',
                 font_size, atlas_width, traceMilliseconds());
    std::fflush(stderr);
  }
  static constexpr int kIndicesPerQuad = 6;

  /// THE CEILING A QUAD BATCH CANNOT SEE PAST, and it is arithmetic rather than a policy.
  ///
  /// `initTransientQuadBuffers` writes its indices into a `uint16_t` buffer, quad `i` addressing
  /// vertices `4i .. 4i+3`. So the highest quad that can be addressed is index 16383, holding
  /// vertices 65532..65535 — and one more than that wraps. Quad 16384 asks for 65536..65539,
  /// which truncate to 0..3. Silently: no cap, no error and no assert anywhere on that path.
  ///
  /// WHAT IT WOULD ACTUALLY LOOK LIKE, since assuming worse cost real time. Both buffers ARE
  /// allocated at the full requested size, so nothing is written out of bounds — only the index
  /// VALUE wraps, and every vertex a wrapped index points at was written earlier in the same pass.
  /// The tail of the batch would redraw the HEAD: duplicate geometry and missing tail shapes, not
  /// garbage triangles. If you are chasing stray geometry, it is not this.
  ///
  /// AND IT IS NOT REACHABLE ON A DEFAULT BUILD, which is the more useful fact. The transient
  /// vertex arena runs out first — see traceBatchDropped below — at around 14000 quads for a
  /// 112-byte ShapeVertex. Raising the arena is what would make this ceiling matter.
  ///
  /// Mind which number is which, because they are one apart and either confusion is a real bug:
  /// 16383 is the highest quad INDEX, 16384 is the highest quad COUNT that fits.
  static constexpr int kMaxQuadsPerBatch = 65536 / kVerticesPerQuad;

  /// Where a batch stops being comfortable, as a percentage of whichever ceiling will really bite.
  ///
  /// A FRACTION RATHER THAN A COUNT, and that is not tidiness. The first version warned at a fixed
  /// 15000, which on a default build is ABOVE the transient arena's real capacity of about
  /// 14000 quads — so the warning could never fire before the batch was already dead. A threshold
  /// that only triggers after the failure it is warning about is worse than none, because it reads
  /// as evidence that nothing was approaching.
  static constexpr int kQuadWarnPercent = 85;

  /// The trace's gate, hoisted so a caller can skip work it only needs when tracing — asking bgfx
  /// how much transient memory is left is cheap, but not so cheap it should happen per batch per
  /// frame in a shipping build that is not being traced.
  inline bool batchTraceEnabled() {
    static const bool enabled = diagnosticBuild() || std::getenv("VISAGE_TRACE_BATCH") != nullptr;
    return enabled;
  }

  /// QUADS HANDED TO THE BATCHER SINCE THIS FRAME BEGAN, across every batch in it.
  ///
  /// Not gated on anything, unlike the trace, because the interesting quantity is a per-FRAME total
  /// and a batch that declined to report is still one that consumed arena. An atomic add per batch,
  /// a handful per frame, sits far below noise — and having the number always available is what
  /// makes it possible to size the arena from measurement instead of from a guess.
  ///
  /// `Canvas::submit` zeroes it as a frame begins, so a caller reads the frame it just submitted.
  inline std::atomic<uint64_t>& batchQuadCounter() {
    static std::atomic<uint64_t> counter { 0 };
    return counter;
  }

  inline void countBatchQuads(int num_quads) {
    batchQuadCounter().fetch_add(static_cast<uint64_t>(num_quads), std::memory_order_relaxed);
  }

  inline void resetBatchQuadCount() {
    batchQuadCounter().store(0, std::memory_order_relaxed);
  }

  inline uint64_t batchQuadsThisFrame() {
    return batchQuadCounter().load(std::memory_order_relaxed);
  }

  /// OPT-IN BATCH TRACE, sibling of the atlas trace above and gated the same way: off unless
  /// VISAGE_TRACE_BATCH is set or this is a diagnostic build, read once into a function-local
  /// static, so a build with neither pays one predictable branch on a path that already allocates
  /// GPU buffers.
  ///
  /// TWO HALVES, DELIBERATELY DIFFERENT. The warning half is opt-in — a big batch is interesting,
  /// not wrong, and nobody wants that line in an ordinary session. The overflow half is not: by
  /// the time it fires the frame is already corrupt, and a corruption that only reports in a build
  /// you must first reproduce it in is a corruption you mostly do not catch. So it prints in
  /// release too, and asserts on top of that in debug.
  ///
  /// THOUGH NOT WHEN THE TRACE IS ON. Somebody who set VISAGE_TRACE_BATCH asked to WATCH overflows
  /// happen, and trapping on the first one is the opposite of that — it would also make an
  /// overflow impossible to exercise deliberately in a debug build, which is exactly what a
  /// control has to do. The line is printed either way; only the trap is disarmed.
  ///
  /// AND THE REPORT IS BOUNDED. An overflowing batch is normally a per-frame condition, so an
  /// ungated report would be sixty lines a second into somebody's log. After a handful it says so
  /// and goes quiet. The opt-in half is not bounded: whoever turned it on wants all of them.
  ///
  /// `available_quads` is what the transient arena had left when this batch asked, or -1 when the
  /// caller did not look — which it only does while tracing. The warning is measured against
  /// whichever of the two ceilings is lower, because that is the one the batch will meet.
  inline void traceBatchQuads(std::string_view which, int num_quads, int available_quads) {
    const bool enabled = batchTraceEnabled();

    if (num_quads > kMaxQuadsPerBatch) {
      static constexpr int kMaxOverflowReports = 8;
      static std::atomic<int> reports { 0 };
      const int seen = reports.load(std::memory_order_relaxed);
      if (seen < kMaxOverflowReports) {
        reports.store(seen + 1, std::memory_order_relaxed);
        const bool last = seen + 1 == kMaxOverflowReports;
        std::fprintf(stderr,
                     "[VISAGE-BATCH] OVERFLOW %.*s %d quads > %d, quad %d on wraps to 0%s\n",
                     static_cast<int>(which.size()), which.data(), num_quads, kMaxQuadsPerBatch,
                     kMaxQuadsPerBatch, last ? " (further reports suppressed)" : "");
        std::fflush(stderr);
      }
      if (!enabled) {
        VISAGE_ASSERT(num_quads <= kMaxQuadsPerBatch);
      }
      return;
    }

    if (!enabled) {
      return;
    }

    const bool arena_is_lower = available_quads >= 0 && available_quads < kMaxQuadsPerBatch;
    const int ceiling = arena_is_lower ? available_quads : kMaxQuadsPerBatch;
    if (num_quads > ceiling * kQuadWarnPercent / 100) {
      std::fprintf(stderr, "[VISAGE-BATCH] warn %.*s %d quads, ceiling %d (index %d, arena %d)\n",
                   static_cast<int>(which.size()), which.data(), num_quads, ceiling,
                   kMaxQuadsPerBatch, available_quads);
      std::fflush(stderr);
    }
  }

  /// A BATCH THAT HAD TO BE SPLIT, which is no longer a fault — it is a cost.
  ///
  /// Past 16384 quads a batch is submitted in runs rather than wrapping its indices, so the frame
  /// is correct and pays an extra draw call per run. Worth seeing while tracing, because the
  /// per-batch total is otherwise invisible once it has been chunked: every report below this point
  /// describes a RUN, and a run at the ceiling could be one of two or one of twenty.
  ///
  /// Opt-in only. Nothing is wrong when this fires.
  inline void traceBatchSplit(std::string_view which, int num_quads, int runs) {
    if (!batchTraceEnabled()) {
      return;
    }
    std::fprintf(stderr, "[VISAGE-BATCH] SPLIT %.*s %d quads into %d runs of at most %d\n",
                 static_cast<int>(which.size()), which.data(), num_quads, runs, kMaxQuadsPerBatch);
    std::fflush(stderr);
  }

  /// FEWER QUADS WRITTEN THAN THE BATCH ALLOCATED AND INDEXED, which is the one arrangement that
  /// puts GARBAGE on the screen rather than something missing.
  ///
  /// `initTransientQuadBuffers` generates indices for every quad it allocated. If the writer then
  /// fills fewer, the tail indices point at vertices nobody wrote this frame — whatever the transient
  /// arena happens to hold, reinterpreted as positions. That draws stray triangles at arbitrary
  /// coordinates and, because consecutive stale vertices differ progressively, sheared streaks of
  /// whatever was there before.
  ///
  /// visage asserts this invariant in three places and every one of them is compiled out under
  /// NDEBUG, so a release build has been unable to say it. That is why this reports rather than
  /// asserts: the frames where it would matter are somebody's session, not a debug run.
  ///
  /// THE CALLER IS EXPECTED TO ZERO THE TAIL after calling this. A zeroed vertex has no position and
  /// no dimension, so its triangles have no area and draw nothing — which turns garbage geometry into
  /// missing geometry. Missing is a bug; garbage is a bug that looks like hardware failure.
  inline void traceBatchShort(std::string_view which, int written, int allocated) {
    static constexpr int kMaxShortReports = 8;
    static std::atomic<int> reports { 0 };
    const int seen = reports.load(std::memory_order_relaxed);
    if (seen >= kMaxShortReports) {
      return;
    }

    reports.store(seen + 1, std::memory_order_relaxed);
    const bool last = seen + 1 == kMaxShortReports;
    std::fprintf(stderr, "[VISAGE-BATCH] SHORT %.*s wrote %d of %d quads - tail zeroed%s\n",
                 static_cast<int>(which.size()), which.data(), written, allocated,
                 last ? " (further reports suppressed)" : "");
    std::fflush(stderr);
  }

  /// THE WALK STILL HAD PIECES WHEN THE COUNT WAS SATISFIED — the other way `numShapes` and
  /// `fillQuadChunk` can disagree, and the one a resume bug takes.
  ///
  /// The two share a predicate precisely so they cannot differ, and a chunked batch is the only
  /// place that relies on it: the count decides how many runs to make, and the cursor decides which
  /// pieces go in them. A cursor that loses its place at a run boundary — resuming a shape at its
  /// first damage rectangle rather than the rectangle it stopped on — writes one piece twice and
  /// then runs out of room for the last one, while EVERY RUN STILL WRITES EXACTLY THE CHUNK IT WAS
  /// ASKED FOR. So `traceBatchShort` cannot see it and neither can the pixels, in a retained-mode
  /// UI where the dropped piece's rectangle keeps last frame's ink.
  ///
  /// What gives it away is the cursor: after the final run the walk must be finished. This says so
  /// when it is not, and reports in release for the same reason the drop does — the frames where it
  /// would matter are somebody's session.
  inline void traceBatchUnwalked(std::string_view which, int counted) {
    static constexpr int kMaxUnwalkedReports = 8;
    static std::atomic<int> reports { 0 };
    const int seen = reports.load(std::memory_order_relaxed);
    if (seen >= kMaxUnwalkedReports) {
      return;
    }

    reports.store(seen + 1, std::memory_order_relaxed);
    const bool last = seen + 1 == kMaxUnwalkedReports;
    std::fprintf(stderr,
                 "[VISAGE-BATCH] UNWALKED %.*s counted %d quads, walk not finished - pieces "
                 "dropped%s\n",
                 static_cast<int>(which.size()), which.data(), counted,
                 last ? " (further reports suppressed)" : "");
    std::fflush(stderr);
  }

  /// A SHAPE AT A COORDINATE NOBODY COULD HAVE MEANT — the failure class every other report here is
  /// blind to, because nothing about it is inconsistent.
  ///
  /// SHORT, UNWALKED, DROPPED and OVERFLOW all detect a disagreement: a count against a walk, a
  /// request against an arena. A shape whose POSITION is wrong disagrees with nothing. It is counted
  /// once, walked once, written once and drawn once — correctly, at the wrong place. Every log stays
  /// clean and the frame is still wrong, which is exactly what a silent tripwire log beside a
  /// screenshot of a glitch means.
  ///
  /// visage never checked this. A non-finite coordinate reaching `setQuadPositions` writes NaN or
  /// infinity into vertex positions, and what the GPU does with those is undefined — in practice a
  /// triangle stretched across the viewport, which is the filled-wedge costume. A finite but wrong
  /// coordinate is quieter and just as wrong.
  ///
  /// Reports the shape's own rectangle so the line names WHICH geometry, not merely that some
  /// existed. Release, bounded at eight, like its neighbours.
  inline void traceBatchWildPosition(std::string_view which, float x, float y, float width, float height) {
    static constexpr int kMaxWildReports = 8;
    static std::atomic<int> reports { 0 };
    const int seen = reports.load(std::memory_order_relaxed);
    if (seen >= kMaxWildReports) {
      return;
    }

    reports.store(seen + 1, std::memory_order_relaxed);
    const bool last = seen + 1 == kMaxWildReports;
    std::fprintf(stderr,
                 "[VISAGE-BATCH] WILD %.*s at (%g, %g) size (%g x %g) - not a finite rectangle%s\n",
                 static_cast<int>(which.size()), which.data(), static_cast<double>(x),
                 static_cast<double>(y), static_cast<double>(width), static_cast<double>(height),
                 last ? " (further reports suppressed)" : "");
    std::fflush(stderr);
  }

  /// WHEN THE CORNER LAST REPAINTED — the line that separates ink written ONCE AND KEPT from ink
  /// drawn fresh every frame, without having to catch the draw itself.
  ///
  /// THIS RENDERER CARRIES PIXELS FORWARD, which is the fact the whole line rests on. There is no
  /// `setViewClear` anywhere in this fork; `Layer::clearInvalidRectAreas` paints opaque fills over the
  /// invalid rectangles and over nothing else. Every pixel outside a frame's damage is therefore last
  /// frame's pixel, indefinitely — so ONE bad draw is enough to put something on screen permanently.
  /// It survives until something damages that rectangle and vanishes exactly when something does,
  /// which is what a glitch that disappears under a passing panel actually is.
  ///
  /// So the useful question is not what drew the ink but WHEN THAT AREA WAS LAST REPAINTED. Read
  /// against a screenshot's timestamp, these lines say whether the corner was being redrawn while the
  /// artifact was visible, which decides between the two models on their own.
  ///
  /// EDGE-TRIGGERED RATHER THAN PER FRAME, because a drag repaints continuously and sixty lines a
  /// second would bury the log this is meant to make readable. One line when the corner starts
  /// repainting, one when it stops, carrying the run length — so a burst costs two lines whether it
  /// spans three frames or three thousand, and the GAP between two bursts is exactly the window in
  /// which anything drawn there was preserved untouched.

  /// THE OTHER ATLAS — the one nothing here has ever traced, and a different beast from the font's.
  ///
  /// An intermediate layer packs its REGIONS into a single texture, and `coordinatesForRegion`
  /// returns a region's slot in that texture rather than a window position. A region that needs a
  /// layer is re-packed on every `setBounds`, because `setupIntermediateRegion` calls
  /// `changePackedLayer`, which is a remove followed by an add — so a mere resize hands the region a
  /// NEW SLOT in the layer texture.
  ///
  /// WHY THAT DESERVES A LINE. `addPackedRegion` invalidates only when the add FAILS and forces a
  /// repack. When the region fits, it is quietly given a fresh slot and NOTHING IS MARKED DIRTY — and
  /// the layer texture has never been drawn at that slot, so a composite reading from it reads
  /// whatever the texture happened to hold. Uninitialised texture memory composited into a window is
  /// dense noise, in a rectangle, clipped to wherever that composite lands.
  ///
  /// So the line carries the slot, whether a repack happened, and — the load-bearing field — whether
  /// the region is DIRTY afterwards. A slot change with `dirty 0` means a composite is about to read
  /// pixels nobody ever wrote.
  ///
  /// Unbounded: regions are packed on layout and on resize, tens of times in a session, never per
  /// frame.
  inline void traceLayerRegionPacked(bool intermediate, int width, int height, int slotX, int slotY,
                                     bool repacked, bool dirty) {
    if (!batchTraceEnabled()) {
      return;
    }
    // WHICH LAYER, because only an INTERMEDIATE one composites from these slots. The main window
    // layer keeps an atlas map too and never reads it — `boundsForRegion` returns window coordinates
    // there — so a line without this word invites reading ordinary setup as the fault.
    std::fprintf(stderr,
                 "[VISAGE-LAYER] %s region %dx%d -> slot (%d, %d) repacked %d dirty %d at %lld\n",
                 intermediate ? "intermediate" : "window", width, height, slotX, slotY,
                 repacked ? 1 : 0, dirty ? 1 : 0, traceMilliseconds());
    std::fflush(stderr);
  }

  /// A REGION LEAVING THE LAYER ATLAS. Paired with the line above so that a remove-then-add — which
  /// is what every re-bounds of a layer-backed region actually is — reads as the two events it is,
  /// rather than as one unexplained slot change.
  inline void traceLayerRegionRemoved(bool intermediate, int width, int height) {
    if (!batchTraceEnabled()) {
      return;
    }
    std::fprintf(stderr, "[VISAGE-LAYER] %s region %dx%d removed at %lld\n",
                 intermediate ? "intermediate" : "window", width, height, traceMilliseconds());
    std::fflush(stderr);
  }

  /// THE LAYER TEXTURE'S OWN LIFE — created, destroyed, resized — because a reallocation is the one
  /// event that changes a texture's CONTENTS without any draw, any damage, or any packing.
  ///
  /// A new bgfx texture is uninitialised VRAM. Under partial damage only subsequently-dirtied
  /// rectangles are redrawn into it, so everything undamaged keeps whatever the allocator handed
  /// over — permanently, and with no trace anywhere else in this file. One no-draw, no-damage event
  /// producing a frozen wedge is exactly the shape being hunted.
  ///
  /// THE FIELD TO READ IS `invalid`, and the reason is a real asymmetry in Layer. `setDimensions`
  /// destroys the frame buffer AND invalidates, which is safe — everything redraws into the new
  /// texture. But `destroyFrameBuffer` is also called on its own by `setHdr`, and by `pairToWindow`
  /// and `setWindowlessRender` AFTER a `setDimensions` that EARLY-RETURNS when the dimensions have
  /// not changed — so the invalidate never runs while the destroy always does. A layer whose texture
  /// is gone and which has nothing marked dirty will not redraw itself: `Layer::submit` early-returns
  /// on no invalid rects, so `checkFrameBuffer` never runs, and whatever samples that layer samples a
  /// handle that is no longer valid.
  ///
  /// So a `destroy` line with `invalid 0` is the smoking gun, and a HANDLE that changes between two
  /// frames is a reallocation whether or not anything else reported one.
  ///
  /// On the batch gate rather than its own, because these are a handful of lines per session.
  inline void traceLayerTexture(const char* event, bool intermediate, int fromWidth, int fromHeight,
                                int toWidth, int toHeight, int handleIndex, bool anyInvalid) {
    if (!batchTraceEnabled()) {
      return;
    }
    std::fprintf(stderr,
                 "[VISAGE-TEX] %s %s %dx%d -> %dx%d handle %d invalid %d at %lld\n",
                 intermediate ? "intermediate" : "window", event, fromWidth, fromHeight, toWidth,
                 toHeight, handleIndex, anyInvalid ? 1 : 0, traceMilliseconds());
    std::fflush(stderr);
  }

  /// EVERY COMPOSITE OF A LAYER INTO ANOTHER — the one path that puts pixels on screen WITHOUT being
  /// a shape in the damage-clamped batcher walk, and therefore the one path every tripwire here is
  /// blind to.
  ///
  /// WHY THIS EXISTS. A wedge was photographed with ORIGIN silent, WILD silent, no atlas event within
  /// 32 seconds, no layer-atlas event within 40, and no damage on its corner within 33. In a renderer
  /// that carries pixels forward, something wrote to the surface entirely outside the damage system.
  /// A composite is that: `Canvas::submit` invalidates the composite layer and resubmits it EVERY
  /// frame regardless of damage, so a blit runs whether or not anything asked for one.
  ///
  /// WHAT IT PRINTS AND WHY EACH FIELD IS THERE. The destination rectangle in the space it is drawn
  /// in; the SOURCE rectangle, which is `coordinatesForRegion` output and is ATLAS-PACKED for an
  /// intermediate layer and a window position otherwise — the two spaces whose crossing is the
  /// suspected fault; and the source layer's dimensions, because the shader divides by them
  /// (`kAtlasScale`), so a rectangle correct in pixels is still wrong if it is scaled by the wrong
  /// layer's size. A blit whose source rectangle lies outside its own layer's dimensions is sampling
  /// texture nobody wrote, which is what uninitialised static IS.
  ///
  /// OPT-IN AND VERBOSE, by its own environment variable rather than the diagnostic flag: this fires
  /// per composited region per frame, which is right for a four-minute probe run against a
  /// deterministic reproduction and wrong for anybody's session.
  inline bool blitTraceEnabled() {
    static const bool enabled = std::getenv("VISAGE_TRACE_BLIT") != nullptr;
    return enabled;
  }

  /// ALL FOUR VERTICES, not the two corners, and the reason is the geometry of the thing being
  /// hunted. A quad is drawn as two triangles split on its diagonal — kQuadTriangles is
  /// {0,1,2, 2,1,3} — so vertex 0 belongs ONLY to the first triangle and vertex 3 ONLY to the
  /// second, while vertices 1 and 2 are in BOTH. One corrupt vertex therefore poisons either one
  /// triangle or both, depending on WHICH, and that maps directly onto whether a photographed
  /// artifact is a single wedge with a diagonal hypotenuse or a whole quad.
  ///
  /// An earlier version printed only vertices 0 and 3 — the two corners the rectangle is built from
  /// — which meant the two vertices that poison BOTH triangles were the two it could not see. This
  /// checks the quad is still a rectangle: v1 must share v0's top and v3's left, v2 must share v0's
  /// left and v3's bottom, in position and in texture coordinates alike. Anything else is a vertex
  /// that no longer agrees with the rectangle it was built from.
  ///
  /// AND IT READS THE REAL BUFFER. These vertices are the transient vertex buffer bgfx handed back —
  /// initQuadVertices returns that memory directly — so this is what was WRITTEN for the GPU, not a
  /// restatement of intent. It is written at write time, though: corruption after this point and
  /// before the draw consumes it would still be invisible here.
  struct BlitQuad {
    float x[4];
    float y[4];
    float u[4];
    float v[4];
  };

  inline void traceBlit(bool intermediate, int layerWidth, int layerHeight, const BlitQuad& quad) {
    if (!blitTraceEnabled()) {
      return;
    }

    // NAMED WHEN IT IS ALREADY WRONG, so a reader does not check arithmetic on thousands of lines.
    const bool outside = quad.u[0] < 0 || quad.v[0] < 0 || quad.u[3] > layerWidth ||
                         quad.v[3] > layerHeight;
    const bool malformed = quad.y[1] != quad.y[0] || quad.x[2] != quad.x[0] ||
                           quad.x[1] != quad.x[3] || quad.y[2] != quad.y[3] ||
                           quad.v[1] != quad.v[0] || quad.u[2] != quad.u[0] ||
                           quad.u[1] != quad.u[3] || quad.v[2] != quad.v[3];

    std::fprintf(stderr,
                 "[VISAGE-BLIT] %s src (%g,%g)-(%g,%g) of %dx%d -> dest (%g,%g)-(%g,%g)%s%s at %lld\n",
                 intermediate ? "intermediate" : "window", static_cast<double>(quad.u[0]),
                 static_cast<double>(quad.v[0]), static_cast<double>(quad.u[3]),
                 static_cast<double>(quad.v[3]), layerWidth, layerHeight,
                 static_cast<double>(quad.x[0]), static_cast<double>(quad.y[0]),
                 static_cast<double>(quad.x[3]), static_cast<double>(quad.y[3]),
                 outside ? "  OUTSIDE-SOURCE" : "", malformed ? "  MALFORMED-QUAD" : "",
                 traceMilliseconds());

    // The whole quad, but only when it has stopped being one — otherwise this is four times the log
    // for a rectangle whose corners already said everything.
    if (malformed) {
      for (int i = 0; i < 4; ++i) {
        std::fprintf(stderr, "[VISAGE-BLIT]   v%d pos (%g, %g) uv (%g, %g)\n", i,
                     static_cast<double>(quad.x[i]), static_cast<double>(quad.y[i]),
                     static_cast<double>(quad.u[i]), static_cast<double>(quad.v[i]));
      }
    }
    std::fflush(stderr);
  }

  /// A LAYER BEING CREATED, which the packing trace above cannot show: a region joining an existing
  /// layer's atlas is one event, a whole new layer coming into existence is another, and only the
  /// first was ever reported. If a subtree is promoted to its own layer mid-session — the classic
  /// reason being an opacity animation — this is the line that says so.
  inline void traceLayerCreated(int index) {
    if (!batchTraceEnabled()) {
      return;
    }
    std::fprintf(stderr, "[VISAGE-LAYER] layer %d CREATED at %lld\n", index, traceMilliseconds());
    std::fflush(stderr);
  }

  /// WHAT THE LAST FRAME ACTUALLY REPAINTED, readable by a probe rather than only printable.
  ///
  /// The edge-triggered line below is right for a session log and useless to a harness photographing
  /// every frame: a capture loop needs to record, for the frame it just took, whether that frame
  /// redrew anything and whether it touched the area under suspicion. This is that, updated once per
  /// `Canvas::submit` and readable straight afterwards.
  ///
  /// AND IT ANSWERS THE FIRST QUESTION A NEW VENUE HAS TO ASK ITSELF: does it carry pixels forward at
  /// all? A harness that full-redraws every present cannot reproduce a PINNED artifact however
  /// faithfully it stages the corruption, and would report clean frames forever while the venue —
  /// not the code — was the reason. Damage falling to zero on quiet frames means the retention is
  /// real and partial. A venue repainting everything every frame reports every region damaged,
  /// always, and pinning cannot be tested there.
  ///
  /// Not gated on the trace: a probe reading these should not have to also be logging.
  struct FrameDamage {
    int regions = 0;      ///< regions with any damage this frame
    int rects = 0;        ///< damage rectangles across all of them
    bool corner = false;  ///< did any rectangle reach the top-left 100x100
  };

  inline FrameDamage& lastFrameDamage() {
    static FrameDamage damage;
    return damage;
  }

  inline void traceCornerDamage(bool damagedThisFrame) {
    if (!batchTraceEnabled()) {
      return;
    }

    // Main thread only, like the whole submit path this is called from.
    static bool repainting = false;
    static long long runFrames = 0;

    if (damagedThisFrame) {
      runFrames++;
      if (!repainting) {
        repainting = true;
        std::fprintf(stderr, "[VISAGE-DAMAGE] corner repaint BEGAN at %lld\n", traceMilliseconds());
        std::fflush(stderr);
      }
      return;
    }

    if (repainting) {
      repainting = false;
      std::fprintf(stderr, "[VISAGE-DAMAGE] corner repaint ENDED after %lld frames at %lld\n",
                   runFrames, traceMilliseconds());
      std::fflush(stderr);
      runFrames = 0;
    }
  }

  /// A QUAD IN THE TOP-LEFT CORNER THAT ITS REGION CANNOT ACCOUNT FOR — the aimed half of WILD
  /// above, pointed at the one costume that keeps getting photographed.
  ///
  /// WILD catches a coordinate that is not a number. This catches one that IS: a shape landing in
  /// the top-left 100x100 of the surface it renders into, while the region drawing it sits somewhere
  /// else entirely. That is the arithmetic of a caller handing over an ABSOLUTE zero where a
  /// region-local coordinate was wanted — the shape's own x is then exactly minus its region's — and
  /// it is why both halves are printed rather than their sum. A local of (-516, -968) under a region
  /// at (516, 968) names the mistake outright; a local of (0, 0) under a region reported at the
  /// origin would mean something quite different and is not this.
  ///
  /// CHECKED AFTER THE CLAMP, which is what keeps it from being noise. A shape reaching out of its
  /// region towards the corner is normally refused by `totallyClamped` and never reaches here at
  /// all; only geometry that survives clamping — geometry that will actually reach pixels — is
  /// examined. That is the same set as the geometry in the screenshots.
  ///
  /// WHAT IT CANNOT SEE, which matters more than what it can, because a silent tripwire gets read as
  /// an alibi. A region that is ITSELF at the top-left draws there legitimately all day — the root
  /// region and the window background both are — so such regions are excluded wholesale, and a fault
  /// inside one of them passes unremarked. Silence here NARROWS the suspects. It does not clear
  /// them.
  ///
  /// Release, and bounded like its neighbours but at 64 rather than 8: this fault redraws every
  /// frame once it starts, and eight lines could not show whether it began with a gesture.
  inline void traceBatchOriginPosition(std::string_view which, float x, float y, float width,
                                       float height, int region_x, int region_y) {
    static constexpr int kMaxOriginReports = 64;
    static std::atomic<int> reports { 0 };
    const int seen = reports.load(std::memory_order_relaxed);
    if (seen >= kMaxOriginReports) {
      return;
    }

    reports.store(seen + 1, std::memory_order_relaxed);
    const bool last = seen + 1 == kMaxOriginReports;
    std::fprintf(stderr,
                 "[VISAGE-BATCH] ORIGIN %.*s local (%g, %g) size (%g x %g) in region at (%d, %d) "
                 "-> draws at (%g, %g) at %lld%s\n",
                 static_cast<int>(which.size()), which.data(), static_cast<double>(x),
                 static_cast<double>(y), static_cast<double>(width), static_cast<double>(height),
                 region_x, region_y, static_cast<double>(x) + region_x,
                 static_cast<double>(y) + region_y, traceMilliseconds(),
                 last ? " (further reports suppressed)" : "");
    std::fflush(stderr);
  }

  /// A WHOLE BATCH GOING MISSING, which is the failure that actually fires — and until now the one
  /// nothing anywhere reported.
  ///
  /// The transient vertex arena is a fixed per-frame pool (bgfx's default is 6 MB, and visage never
  /// sets `Init::limits`). When `allocTransientBuffers` cannot serve a batch it returns false, and
  /// `setupQuads` then skips the batch ENTIRELY — every shape of that type, across every region in
  /// the layer, is simply not drawn that frame. Over a frame buffer that is not cleared, and chrome
  /// that does not repaint, what stays on screen is whatever was underneath.
  ///
  /// MEASURED, because the arithmetic is worth having in front of whoever reads this: a ShapeVertex
  /// is 112 bytes, so the 6 MB pool holds 56173 vertices — 14043 quads, for the WHOLE FRAME, shared
  /// across every batch in it. That is BELOW the 16384 index ceiling, which is why the index wrap
  /// above has never actually been reachable for these shapes. Two surfaces that each sit inside
  /// their own budget can still drop each other's batch.
  ///
  /// NOT AN ASSERT, deliberately, and this is the difference from the wrap. Running out of a
  /// runtime resource is not a programming error, and trapping somebody's DAW for it would be
  /// wrong. It prints, in release, bounded — and then the frame is wrong and at least it said so.
  inline void traceBatchDropped(std::string_view which, int num_quads, int available_quads) {
    static constexpr int kMaxDroppedReports = 8;
    static std::atomic<int> reports { 0 };
    const int seen = reports.load(std::memory_order_relaxed);
    if (seen >= kMaxDroppedReports) {
      return;
    }

    reports.store(seen + 1, std::memory_order_relaxed);
    const bool last = seen + 1 == kMaxDroppedReports;
    std::fprintf(stderr, "[VISAGE-BATCH] DROPPED %.*s %d quads, arena has %d - nothing drawn%s\n",
                 static_cast<int>(which.size()), which.data(), num_quads, available_quads,
                 last ? " (further reports suppressed)" : "");
    std::fflush(stderr);
  }

  bool preprocessWebGlShader(std::string& result, const std::string& code,
                             const std::string& utils_source, const std::string& varying_source);

  const uint16_t kQuadTriangles[] = {
    0, 1, 2, 2, 1, 3,
  };

  struct PackedAtlasData;

  struct PackedRect {
    int x;
    int y;
    int w;
    int h;
  };

  struct TextureRect {
    int left;
    int top;
    int right;
    int bottom;
  };

  class AtlasPacker {
  public:
    AtlasPacker();
    ~AtlasPacker();

    bool addRect(PackedRect& rect);
    void clear();
    bool pack(std::vector<PackedRect>& rects, int width, int height);
    void setPadding(int padding) { padding_ = padding; }
    int padding() const { return padding_; }

    bool packed() const { return packed_; }

  private:
    std::unique_ptr<PackedAtlasData> data_;
    bool packed_ = false;
    int padding_ = 1;
    int rect_index_ = 0;
  };

  template<typename T = int>
  class PackedAtlasMap {
  public:
    static constexpr int kDefaultWidth = 64;

    PackedAtlasMap() = default;

    bool addRect(T id, int width, int height) {
      VISAGE_ASSERT(lookup_.count(id) == 0);

      int index = packed_rects_.size();
      lookup_[id] = index;
      packed_rects_.push_back({ 0, 0, std::max(0, width), std::max(0, height) });
      return packer_.addRect(packed_rects_.back());
    }

    bool hasId(T id) const { return lookup_.count(id) > 0; }

    void removeRect(T id) {
      VISAGE_ASSERT(lookup_.count(id) > 0);
      lookup_.erase(id);
    }

    void pack(int start_width = kDefaultWidth, int start_height = kDefaultWidth) {
      static constexpr int kMaxDimension = 1 << 14;

      checkRemovedRects();
      if (packed_rects_.size() == 1) {
        width_ = std::max(1, packed_rects_[0].w + packer_.padding());
        height_ = std::max(1, packed_rects_[0].h + packer_.padding());
        if (!packer_.pack(packed_rects_, width_, height_))
          VISAGE_ASSERT(false);
      }
      else if (!packed_rects_.empty()) {
        width_ = std::max(kDefaultWidth, start_width);
        height_ = std::max(kDefaultWidth, start_height);

        while (width_ < kMaxDimension * 2 || height_ < kMaxDimension * 2) {
          width_ = fixed_width_ ? fixed_width_ : std::min(kMaxDimension, width_);
          height_ = std::min(kMaxDimension, height_);
          if (packer_.pack(packed_rects_, width_, height_))
            return;

          width_ *= 2;
          height_ *= 2;
        }
        VISAGE_ASSERT(false);
      }
    }

    void clear() {
      lookup_.clear();
      packer_.clear();
      packed_rects_.clear();
    }

    void setPadding(int padding) { packer_.setPadding(padding); }
    int padding() const { return packer_.padding(); }

    const PackedRect& rectAtIndex(int index) const {
      VISAGE_ASSERT(index >= 0 && index < packed_rects_.size());
      return packed_rects_[index];
    }

    TextureRect texturePositionsForIndex(int rect_index, bool bottom_left_origin = false) const {
      const PackedRect& packed_rect = rectAtIndex(rect_index);
      TextureRect result = { packed_rect.x, packed_rect.y, packed_rect.x + packed_rect.w,
                             packed_rect.y + packed_rect.h };

      if (bottom_left_origin) {
        result.top = height_ - result.top;
        result.bottom = height_ - result.bottom;
      }
      return result;
    }

    const PackedRect& rectForId(T id) const {
      VISAGE_ASSERT(lookup_.count(id) > 0);
      return rectAtIndex(lookup_.at(id));
    }

    TextureRect texturePositionsForId(T id, bool bottom_left_origin = false) const {
      VISAGE_ASSERT(lookup_.count(id) > 0);
      return texturePositionsForIndex(lookup_.at(id), bottom_left_origin);
    }

    void fixWidth(int width) { fixed_width_ = width; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool packed() const { return packer_.packed(); }
    int numRects() const { return packed_rects_.size(); }

  private:
    void checkRemovedRects() {
      if (packed_rects_.size() == lookup_.size())
        return;

      std::vector<PackedRect> old_rects = std::move(packed_rects_);
      packed_rects_.reserve(lookup_.size());
      for (auto& packed : lookup_) {
        int index = packed_rects_.size();
        packed_rects_.push_back(old_rects[packed.second]);
        packed.second = index;
      }
    }

    int fixed_width_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::vector<PackedRect> packed_rects_;
    AtlasPacker packer_;
    std::map<T, int> lookup_;
  };

  struct UvVertex {
    float x;
    float y;
    float u;
    float v;

    static bgfx::VertexLayout& layout();
  };

  struct PathVertex {
    float index;
    float direction;
    float x1;
    float y1;
    float x2;
    float y2;
    float x3;
    float y3;

    static bgfx::VertexLayout& layout();
  };

  struct GradientTexturePosition {
    float from_x;
    float from_y;
    float to_x;
    float to_y;
  };

  struct GradientVertexPosition {
    float from_x;
    float from_y;
    float to_x;
    float to_y;
    float coefficient1;
    float coefficient2;
    float coefficient3;
    float cone_height;

    float* position1() { return &from_x; }
    float* position2() { return &coefficient1; }
  };

  struct ShapeVertex {
    float x;
    float y;
    float garbage1;
    float garbage2;
    GradientTexturePosition gradient_texture_position;
    GradientVertexPosition gradient;
    float coordinate_x;
    float coordinate_y;
    float dimension_x;
    float dimension_y;
    float clamp_left;
    float clamp_top;
    float clamp_right;
    float clamp_bottom;
    float thickness;
    float fade;
    float value1;
    float value2;

    static bgfx::VertexLayout& layout();
  };

  struct ComplexShapeVertex {
    float x;
    float y;
    float garbage1;
    float garbage2;
    GradientTexturePosition gradient_texture_position;
    GradientVertexPosition gradient;
    float coordinate_x;
    float coordinate_y;
    float dimension_x;
    float dimension_y;
    float clamp_left;
    float clamp_top;
    float clamp_right;
    float clamp_bottom;
    float thickness;
    float fade;
    float value1;
    float value2;
    float value3;
    float value4;
    float value5;
    float value6;

    static bgfx::VertexLayout& layout();
  };

  struct TextureVertex {
    float x;
    float y;
    float dimension_x;
    float dimension_y;
    GradientTexturePosition gradient_texture_position;
    GradientVertexPosition gradient;
    float texture_x;
    float texture_y;
    float direction_x;
    float direction_y;
    float clamp_left;
    float clamp_top;
    float clamp_right;
    float clamp_bottom;

    static bgfx::VertexLayout& layout();
  };

  struct PostEffectVertex {
    float x;
    float y;
    float dimension_x;
    float dimension_y;
    GradientTexturePosition gradient_texture_position;
    GradientVertexPosition gradient;
    float texture_x;
    float texture_y;
    float value1;
    float value2;
    float clamp_left;
    float clamp_top;
    float clamp_right;
    float clamp_bottom;

    static bgfx::VertexLayout& layout();
  };
}
