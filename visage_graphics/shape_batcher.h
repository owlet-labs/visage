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

#include "graphics_utils.h"
#include "post_effects.h"
#include "shapes.h"
#include "visage_utils/space.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace visage {
  class Shader;

  inline int numShapePieces(const BaseShape& shape, int x, int y, const std::vector<IBounds>& invalid_rects) {
    auto check_overlap = [x, y, &shape](IBounds invalid_rect) {
      ClampBounds clamp = shape.clamp.clamp(invalid_rect.x() - x, invalid_rect.y() - y,
                                            invalid_rect.width(), invalid_rect.height());
      return !shape.totallyClamped(clamp);
    };
    return std::count_if(invalid_rects.begin(), invalid_rects.end(), check_overlap);
  }

  template<typename T>
  int numShapes(const BatchVector<T>& batches) {
    int total_size = 0;
    for (const auto& batch : batches) {
      auto count_pieces = [&batch](int sum, const T& shape) {
        return sum + numShapePieces(shape, batch.x, batch.y, *batch.invalid_rects);
      };
      total_size += std::accumulate(batch.shapes->begin(), batch.shapes->end(), 0, count_pieces);
    }
    return total_size;
  }

  void setUniformDimensions(int width, int height);
  void setOriginFlipUniform(bool origin_flip);
  void setBlendMode(BlendMode draw_state);

  bool initTransientQuadBuffers(int num_quads, const bgfx::VertexLayout& layout,
                                bgfx::TransientVertexBuffer* vertex_buffer,
                                bgfx::TransientIndexBuffer* index_buffer, std::string_view which);
  uint8_t* initQuadVerticesWithLayout(int num_quads, const bgfx::VertexLayout& layout,
                                      std::string_view which);
  template<typename T>
  T* initQuadVertices(int num_quads, std::string_view which) {
    return reinterpret_cast<T*>(initQuadVerticesWithLayout(num_quads, T::layout(), which));
  }

  /// THE BATCH'S OWN NAME, so an overflow line says WHICH kind of shape ran past the ceiling.
  /// That is most of the diagnostic value: a count on its own says a frame is corrupt, and the
  /// type says where to look for the thing that grew.
  ///
  /// Read out of the compiler's signature for this function rather than from `typeid`, which
  /// needs RTTI and yields a mangled name that would then want demangling to be worth printing.
  /// Everything here is constexpr — `find`, `find_first_of` and `substr` all are since C++17 —
  /// so a caller that binds the result to a `static constexpr` pays nothing at runtime for it.
  template<typename T>
  constexpr std::string_view batchTypeName() {
#if defined(__GNUC__) || defined(__clang__)
    constexpr std::string_view marker = "T = ";
    const std::string_view signature = __PRETTY_FUNCTION__;
    const size_t found = signature.find(marker);
    if (found == std::string_view::npos)
      return "shape";

    const size_t begin = found + marker.size();
    const size_t end = signature.find_first_of(";]", begin);
    return signature.substr(begin, end - begin);
#else
    // MSVC spells it __FUNCSIG__ and lays it out differently. Not worth a second parser for a name
    // that only ever appears inside a diagnostic — the count and the ceiling are still right.
    return "shape";
#endif
  }

  void submitShapes(const Layer& layer, const EmbeddedFile& vertex_shader,
                    const EmbeddedFile& fragment_shader, bool radial_gradient, int submit_pass);

  /// How many more ShapeVertex quads the transient arena can serve THIS FRAME.
  ///
  /// Falls as a frame fills up, since the pool is shared by every batch in it — which is the whole
  /// character of this limit and the reason a per-surface budget cannot express it. Quoted in
  /// ShapeVertex quads because that is the widest vertex and so the pessimistic count; a batch of a
  /// narrower vertex type gets more.
  int transientQuadCapacity();

  void setImageAtlasUniform(const BatchVector<ImageWrapper>& batches);
  void setGraphDataUniform(const BatchVector<GraphLineWrapper>& batches);
  void setGraphDataUniform(const BatchVector<GraphFillWrapper>& batches);
  void setHeatMapDataUniform(const BatchVector<HeatMapWrapper>& batches);
  void setPathDataUniform(const BatchVector<PathFillWrapper>& batches);

  void submitText(const BatchVector<TextBlock>& batches, const Layer& layer, int submit_pass);
  void submitShader(const BatchVector<ShaderWrapper>& batches, const Layer& layer, int submit_pass);
  void submitSampleRegions(const BatchVector<SampleRegion>& batches, const Layer& layer, int submit_pass);

  template<typename V>
  struct QuadVertices {
    V* vertices = nullptr;
    int num_shapes = 0;
    bool radial_gradient = false;
  };

  /// Is this shape's rectangle a rectangle? Non-finite coordinates are the ones that draw geometry
  /// nobody asked for, and nothing else in this file would notice them.
  inline bool wildPosition(const BaseShape& shape) {
    return !std::isfinite(shape.x) || !std::isfinite(shape.y) || !std::isfinite(shape.width)
           || !std::isfinite(shape.height);
  }

  template<typename T>
  QuadVertices<typename T::Vertex> setupQuads(const BatchVector<T>& batches) {
    QuadVertices<typename T::Vertex> results;
    results.num_shapes = numShapes(batches);
    if (results.num_shapes == 0)
      return results;

    static constexpr std::string_view kBatchName = batchTypeName<T>();
    results.vertices = initQuadVertices<typename T::Vertex>(results.num_shapes, kBatchName);
    if (results.vertices == nullptr)
      return results;
    int vertex_index = 0;

    for (const auto& batch : batches) {
      for (const T& shape : *batch.shapes) {
        for (const IBounds& invalid_rect : *batch.invalid_rects) {
          ClampBounds clamp = shape.clamp.clamp(invalid_rect.x() - batch.x, invalid_rect.y() - batch.y,
                                                invalid_rect.width(), invalid_rect.height());
          if (shape.totallyClamped(clamp))
            continue;

          if (wildPosition(shape)) {
            traceBatchWildPosition(kBatchName, shape.x, shape.y, shape.width, shape.height);
          }
          clamp = clamp.withOffset(batch.x, batch.y);
          setQuadPositions(results.vertices + vertex_index, shape, clamp, batch.x, batch.y);
          shape.setVertexData(results.vertices + vertex_index);
          results.radial_gradient = shape.radialGradient();
          vertex_index += kVerticesPerQuad;
        }
      }
    }

    VISAGE_ASSERT(vertex_index == results.num_shapes * kVerticesPerQuad);
    const int allocated = results.num_shapes * kVerticesPerQuad;
    if (vertex_index < allocated) {
      traceBatchShort(kBatchName, vertex_index / kVerticesPerQuad, results.num_shapes);
      std::memset(results.vertices + vertex_index, 0,
                  static_cast<size_t>(allocated - vertex_index) * sizeof(typename T::Vertex));
    }
    return results;
  }

  /// How far a chunked walk got: which batch, which shape within it, which invalid rect of that
  /// shape. All three are needed to resume, because a piece is a (shape, rect) pair and a shape
  /// spanning several damage rectangles can straddle a chunk boundary.
  struct QuadCursor {
    size_t batch = 0;
    size_t shape = 0;
    size_t rect = 0;
  };

  /// Writes up to `max_quads` pieces from where the cursor left off, and leaves the cursor on the
  /// first piece it did NOT write. Returns how many it wrote.
  ///
  /// The same walk and the same clamp predicate as `setupQuads`, so the piece it stops on is
  /// exactly the one the next run starts with: nothing is drawn twice and nothing is skipped.
  template<typename T>
  int fillQuadChunk(const BatchVector<T>& batches, QuadCursor& cursor, typename T::Vertex* vertices,
                    int max_quads, bool* radial_gradient) {
    int written = 0;
    // BOTH INNER POSITIONS RESET when the batch advances. The shape loop's own increment already
    // clears the rect on every ordinary exit, so today the second reset never changes an outcome —
    // but "today" rests on the shape loop always being left through its increment, which is not a
    // property this loop can see. A cursor is only correct if every field means what it says at
    // every place it is read, and a stale rect here would skip the first damage rectangle of the
    // next batch's first shape: one piece of one shape, missing, in the failure class that hides
    // behind retained pixels.
    for (; cursor.batch < batches.size(); ++cursor.batch, cursor.shape = 0, cursor.rect = 0) {
      const auto& batch = batches[cursor.batch];
      for (; cursor.shape < batch.shapes->size(); ++cursor.shape, cursor.rect = 0) {
        const T& shape = (*batch.shapes)[cursor.shape];
        for (; cursor.rect < batch.invalid_rects->size(); ++cursor.rect) {
          const IBounds& invalid_rect = (*batch.invalid_rects)[cursor.rect];
          ClampBounds clamp = shape.clamp.clamp(invalid_rect.x() - batch.x, invalid_rect.y() - batch.y,
                                                invalid_rect.width(), invalid_rect.height());
          if (shape.totallyClamped(clamp))
            continue;

          // FULL. Returning HERE — before the write, and before the cursor advances past this piece
          // — is what makes the next run resume on this very piece rather than the one after it.
          if (written == max_quads)
            return written;

          if (wildPosition(shape)) {
            traceBatchWildPosition(batchTypeName<T>(), shape.x, shape.y, shape.width, shape.height);
          }
          clamp = clamp.withOffset(batch.x, batch.y);
          setQuadPositions(vertices + written * kVerticesPerQuad, shape, clamp, batch.x, batch.y);
          shape.setVertexData(vertices + written * kVerticesPerQuad);
          *radial_gradient = shape.radialGradient();
          ++written;
        }
      }
    }
    return written;
  }

  /// SPLIT AT THE INDEX CEILING, because one draw call cannot address more than 16384 quads.
  ///
  /// Quad indices are `uint16_t` (see `initTransientQuadBuffers`), so a batch past that wraps and
  /// redraws its own head in place of its tail. Rather than cap the batch — which would drop
  /// geometry just as silently — it is submitted in runs of at most 16384. The cost is one extra
  /// draw call per run, paid only by batches that would otherwise have come out wrong.
  ///
  /// `prepare` re-applies whatever uniforms and textures this shape type needs, ONCE PER RUN, and
  /// that is not optional: `bgfx::submit` defaults to BGFX_DISCARD_ALL, so everything bound for a
  /// draw is gone after it. Binding before the loop would leave every run after the first drawing
  /// with nothing bound.
  ///
  /// A run that cannot allocate stops the walk and keeps what was already drawn. Partial geometry
  /// is not good, but it beats losing the whole batch, and the failure reports itself.
  template<typename T, typename Prepare>
  static void submitBaseShapes(const BatchVector<T>& batches, BlendMode state, Layer& layer,
                               int submit_pass, Prepare&& prepare) {
    const int total = numShapes(batches);
    if (total == 0)
      return;

    static constexpr std::string_view kBatchName = batchTypeName<T>();
    if (total > kMaxQuadsPerBatch) {
      traceBatchSplit(kBatchName, total, (total + kMaxQuadsPerBatch - 1) / kMaxQuadsPerBatch);
    }

    QuadCursor cursor;
    for (int done = 0; done < total;) {
      const int remaining = total - done;
      const int chunk = remaining < kMaxQuadsPerBatch ? remaining : kMaxQuadsPerBatch;
      typename T::Vertex* vertices = initQuadVertices<typename T::Vertex>(chunk, kBatchName);
      if (vertices == nullptr)
        return;

      bool radial_gradient = false;
      const int written = fillQuadChunk(batches, cursor, vertices, chunk, &radial_gradient);
      VISAGE_ASSERT(written == chunk);
      if (written < chunk) {
        // INDEXED BUT NEVER WRITTEN is the one shape of failure that draws garbage rather than
        // nothing: the tail indices point at whatever the transient arena still held. Zeroing gives
        // those quads no position and no dimension, so they cover no pixels.
        traceBatchShort(kBatchName, written, chunk);
        std::memset(vertices + written * kVerticesPerQuad, 0,
                    static_cast<size_t>(chunk - written) * kVerticesPerQuad *
                        sizeof(typename T::Vertex));
      }

      // ADVANCE BY THE CHUNK, not by what was written. The walk is finished either way — a short
      // write means the count disagreed with the writer, and repeating the same chunk would spin
      // forever on a cursor that has nothing left to give.
      done += chunk;

      prepare();
      setBlendMode(state);
      submitShapes(layer, T::vertexShader(), T::fragmentShader(), radial_gradient, submit_pass);
    }

    // AND THE WALK MUST BE FINISHED. The count above decided how many runs to make; the cursor
    // decided what went in them. If pieces remain after the last run, the two disagreed — and the
    // remainder was never drawn. See traceBatchUnwalked for why nothing else here can see it.
    if (cursor.batch < batches.size()) {
      traceBatchUnwalked(kBatchName, total);
    }
  }

  template<typename T>
  static void submitBaseShapes(const BatchVector<T>& batches, BlendMode state, Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass, [] { });
  }

  template<typename T>
  static void submitShapes(const BatchVector<T>& batches, BlendMode state, Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass);
  }

  template<>
  inline void submitShapes<PathFillWrapper>(const BatchVector<PathFillWrapper>& batches,
                                            BlendMode state, Layer& layer, int submit_pass) {
    // PER RUN, not once: a chunked batch submits more than one draw and bgfx discards bound state
    // after each. See submitBaseShapes.
    submitBaseShapes(batches, state, layer, submit_pass, [&] { setPathDataUniform(batches); });
  }

  template<>
  inline void submitShapes<ImageWrapper>(const BatchVector<ImageWrapper>& batches, BlendMode state,
                                         Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass, [&] { setImageAtlasUniform(batches); });
  }

  template<>
  inline void submitShapes<GraphLineWrapper>(const BatchVector<GraphLineWrapper>& batches,
                                             BlendMode state, Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass, [&] { setGraphDataUniform(batches); });
  }

  template<>
  inline void submitShapes<GraphFillWrapper>(const BatchVector<GraphFillWrapper>& batches,
                                             BlendMode state, Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass, [&] { setGraphDataUniform(batches); });
  }

  template<>
  inline void submitShapes<HeatMapWrapper>(const BatchVector<HeatMapWrapper>& batches,
                                           BlendMode state, Layer& layer, int submit_pass) {
    submitBaseShapes(batches, state, layer, submit_pass, [&] { setHeatMapDataUniform(batches); });
  }

  template<>
  inline void submitShapes<ShaderWrapper>(const BatchVector<ShaderWrapper>& batches,
                                          BlendMode state, Layer& layer, int submit_pass) {
    setBlendMode(state);
    submitShader(batches, layer, submit_pass);
  }

  template<>
  inline void submitShapes<TextBlock>(const BatchVector<TextBlock>& batches, BlendMode state,
                                      Layer& layer, int submit_pass) {
    setBlendMode(state);
    submitText(batches, layer, submit_pass);
  }

  template<>
  inline void submitShapes<SampleRegion>(const BatchVector<SampleRegion>& batches, BlendMode state,
                                         Layer& layer, int submit_pass) {
    PostEffect* post_effect = batches[0].shapes->front().post_effect;

    if (post_effect)
      post_effect->submit(batches, layer, submit_pass);
    else {
      setBlendMode(state);
      submitSampleRegions(batches, layer, submit_pass);
    }
  }

  class SubmitBatch;

  struct PositionedBatch {
    SubmitBatch* batch = nullptr;
    std::vector<IBounds>* invalid_rects {};
    int x = 0;
    int y = 0;
  };

  class SubmitBatch {
  public:
    explicit SubmitBatch(BlendMode blend_mode) : blend_mode_(blend_mode) { }
    virtual ~SubmitBatch() = default;
    virtual void clear() = 0;
    virtual void submit(Layer& layer, int submit_pass, const std::vector<PositionedBatch>& others) = 0;

    bool overlapsShape(const BaseShape& shape) const {
      int x = shape.x;
      int y = shape.y;
      int right = shape.x + shape.width;
      int bottom = shape.y + shape.height;
      return std::any_of(areas_.begin(), areas_.end(), [x, y, right, bottom](auto& area) {
        return x < area.right && right > area.x && y < area.bottom && bottom > area.y;
      });
    }

    const void* id() const { return id_; }
    bool match(const void* id, BlendMode blend_mode, bool radial_gradient) const {
      return id_ == id && blend_mode_ == blend_mode && radial_gradient_ == radial_gradient;
    }
    bool match(const SubmitBatch* other) const { return compare(other) == 0; }

    void setBlendMode(BlendMode blend_mode) { blend_mode_ = blend_mode; }
    BlendMode blendMode() const { return blend_mode_; }
    bool radialGradient() const { return radial_gradient_; }

    int compare(const SubmitBatch* other) const {
      if (other == nullptr)
        return 1;

      if (id_ < other->id_)
        return -1;
      if (id_ > other->id_)
        return 1;
      if (blend_mode_ < other->blend_mode_)
        return -1;
      if (blend_mode_ > other->blend_mode_)
        return 1;
      if (radial_gradient_ < other->radial_gradient_)
        return -1;
      if (radial_gradient_ > other->radial_gradient_)
        return 1;
      return 0;
    }

    void clearAreas() { areas_.clear(); }
    void addShapeArea(const BaseShape& shape) {
      VISAGE_ASSERT(id_ == nullptr || id_ == shape.batch_id);
      id_ = shape.batch_id;
      radial_gradient_ = shape.radialGradient();
      areas_.push_back({ shape.x, shape.y, shape.x + shape.width, shape.y + shape.height });
    }

  private:
    struct Area {
      float x, y, right, bottom;
    };

    const void* id_ = nullptr;
    std::vector<Area> areas_;
    BlendMode blend_mode_;
    bool radial_gradient_ = false;
  };

  template<typename T>
  class ShapeBatch : public SubmitBatch {
  public:
    explicit ShapeBatch(BlendMode blend_mode) : SubmitBatch(blend_mode) { }
    ~ShapeBatch() override = default;

    void clear() override {
      clearAreas();
      shapes_.clear();
    }

    void submit(Layer& layer, int submit_pass, const std::vector<PositionedBatch>& batches) override {
      BatchVector<T> batch_list;
      batch_list.reserve(batches.size());
      for (const PositionedBatch& batch : batches) {
        VISAGE_ASSERT(batch.batch->id() == id());
        const std::vector<T>* shapes = &reinterpret_cast<ShapeBatch<T>*>(batch.batch)->shapes_;
        batch_list.emplace_back(shapes, batch.invalid_rects, batch.x, batch.y);
      }
      submitShapes(batch_list, blendMode(), layer, submit_pass);
    }

    void addShape(T shape) {
      addShapeArea(shape);
      shapes_.push_back(std::move(shape));
    }

  private:
    std::vector<T> shapes_;
  };

  class ShapeBatcher {
  public:
    void clear() {
      for (auto& batch : batches_) {
        batch->clear();
        unused_batches_[batch->id()].push_back(std::move(batch));
      }
      batches_.clear();
    }

    void submit(Layer& layer, int submit_pass) {
      for (auto& batch : batches_)
        batch->submit(layer, submit_pass, {});
    }

    int autoBatchIndex(const BaseShape& shape, BlendMode blend) const {
      int match = batches_.size();
      int insert = batches_.size();
      for (int i = batches_.size() - 1; i >= 0; --i) {
        SubmitBatch* batch = batches_[i].get();
        if (batch->match(shape.batch_id, blend, shape.radialGradient()))
          match = i;
        if (batch->overlapsShape(shape))
          break;
        if (batch->id() > shape.batch_id)
          insert = i;
      }
      if (match < batches_.size())
        return match;
      return insert;
    }

    int manualBatchIndex(const BaseShape& shape) const {
      if (batches_.empty())
        return 0;

      return batches_.size() - 1;
    }

    int batchIndex(const BaseShape& shape, BlendMode blend) const {
      if (manual_batching_)
        return manualBatchIndex(shape);
      return autoBatchIndex(shape, blend);
    }

    template<typename T>
    ShapeBatch<T>* createNewBatch(const void* id, BlendMode blend, int insert_index) {
      if (!unused_batches_[id].empty()) {
        auto batch = std::move(unused_batches_[id].back());
        unused_batches_[id].pop_back();
        batch->setBlendMode(blend);
        batches_.insert(batches_.begin() + insert_index, std::move(batch));
      }
      else
        batches_.insert(batches_.begin() + insert_index, std::make_unique<ShapeBatch<T>>(blend));

      return reinterpret_cast<ShapeBatch<T>*>(batches_[insert_index].get());
    }

    template<typename T>
    void addShape(T shape, BlendMode blend = BlendMode::Alpha) {
      int batch_index = batchIndex(shape, blend);
      bool match = batch_index < batches_.size() &&
                   batches_[batch_index]->match(shape.batch_id, blend, shape.radialGradient());
      ShapeBatch<T>* batch = match ? reinterpret_cast<ShapeBatch<T>*>(batches_[batch_index].get()) :
                                     createNewBatch<T>(shape.batch_id, blend, batch_index);

      batch->addShape(std::move(shape));
    }

    void setManualBatching(bool manual) { manual_batching_ = manual; }

    int numBatches() const { return batches_.size(); }
    bool isEmpty() const { return batches_.empty(); }
    SubmitBatch* batchAtIndex(int index) const { return batches_[index].get(); }

  private:
    std::vector<std::unique_ptr<SubmitBatch>> batches_;
    std::map<const void*, std::vector<std::unique_ptr<SubmitBatch>>> unused_batches_;
    bool manual_batching_ = false;
  };
}