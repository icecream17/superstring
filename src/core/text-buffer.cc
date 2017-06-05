#include "text-slice.h"
#include "text-buffer.h"
#include "regex.h"
#include <cassert>
#include <vector>
#include <sstream>

using std::equal;
using std::move;
using std::string;
using std::vector;
using std::u16string;
using String = Text::String;
using MatchResult = Regex::MatchResult;

uint32_t TextBuffer::MAX_CHUNK_SIZE_TO_COPY = 1024;

struct TextBuffer::Layer {
  Layer *previous_layer;
  Patch patch;
  optional<Text> text;
  bool uses_patch;

  Point extent_;
  uint32_t size_;
  uint32_t snapshot_count;

  Layer(Text &&text) :
    previous_layer{nullptr},
    text{move(text)},
    uses_patch{false},
    extent_{this->text->extent()},
    size_{this->text->size()},
    snapshot_count{0} {}

  Layer(Layer *previous_layer) :
    previous_layer{previous_layer},
    patch{Patch()},
    uses_patch{true},
    extent_{previous_layer->extent()},
    size_{previous_layer->size()},
    snapshot_count{0} {}

  static inline Point previous_column(Point position) {
    return Point(position.row, position.column - 1);
  }

  bool is_above_layer(const Layer *layer) const {
    Layer *predecessor = previous_layer;
    while (predecessor) {
      if (predecessor == layer) return true;
      predecessor = predecessor->previous_layer;
    }
    return false;
  }

  uint16_t character_at(Point position) const {
    if (!uses_patch) return text->at(position);

    auto change = patch.get_change_starting_before_new_position(position);
    if (!change) return previous_layer->character_at(position);
    if (position < change->new_end) {
      return change->new_text->at(position.traversal(change->new_start));
    } else {
      return previous_layer->character_at(
        change->old_end.traverse(position.traversal(change->new_end))
      );
    }
  }

  ClipResult clip_position(Point position, bool splay = false) {
    if (!uses_patch) return text->clip_position(position);
    if (snapshot_count > 0) splay = false;

    auto preceding_change = splay ?
      patch.grab_change_starting_before_new_position(position) :
      patch.get_change_starting_before_new_position(position);
    if (!preceding_change) return previous_layer->clip_position(position);

    uint32_t preceding_change_base_offset =
      previous_layer->clip_position(preceding_change->old_start).offset;
    uint32_t preceding_change_current_offset =
      preceding_change_base_offset +
      preceding_change->preceding_new_text_size -
      preceding_change->preceding_old_text_size;

    if (position < preceding_change->new_end) {
      ClipResult position_within_preceding_change =
        preceding_change->new_text->clip_position(
          position.traversal(preceding_change->new_start)
        );

      if (position_within_preceding_change.offset == 0 && preceding_change->old_start.column > 0) {
        if (preceding_change->new_text->content.front() == '\n' &&
            previous_layer->character_at(previous_column(preceding_change->old_start)) == '\r') {
          return {
            previous_column(preceding_change->new_start),
            preceding_change_current_offset - 1
          };
        }
      }

      return {
        preceding_change->new_start.traverse(position_within_preceding_change.position),
        preceding_change_current_offset + position_within_preceding_change.offset
      };
    } else {
      ClipResult base_location = previous_layer->clip_position(
        preceding_change->old_end.traverse(position.traversal(preceding_change->new_end))
      );

      ClipResult distance_past_preceding_change = {
        base_location.position.traversal(preceding_change->old_end),
        base_location.offset - (preceding_change_base_offset + preceding_change->old_text_size)
      };

      if (distance_past_preceding_change.offset == 0 && base_location.offset < previous_layer->size()) {
        uint16_t previous_character = 0;
        if (preceding_change->new_text->size() > 0) {
          previous_character = preceding_change->new_text->content.back();
        } else if (preceding_change->old_start.column > 0) {
          previous_character = previous_layer->character_at(previous_column(preceding_change->old_start));
        }

        if (previous_character == '\r' && previous_layer->character_at(base_location.position) == '\n') {
          return {
            previous_column(preceding_change->new_end),
            preceding_change_current_offset + preceding_change->new_text->size() - 1
          };
        }
      }

      return {
        preceding_change->new_end.traverse(distance_past_preceding_change.position),
        preceding_change_current_offset + preceding_change->new_text->size() + distance_past_preceding_change.offset
      };
    }
  }

  template <typename Callback>
  bool for_each_chunk_in_range(Point start, Point end, const Callback &callback, bool splay = false) {
    Point goal_position = clip_position(end, splay).position;
    Point current_position = clip_position(start, splay).position;

    if (!uses_patch) return callback(TextSlice(*text).slice({current_position, goal_position}));
    if (snapshot_count > 0) splay = false;

    Point base_position;
    auto change = splay ?
      patch.grab_change_starting_before_new_position(current_position) :
      patch.get_change_starting_before_new_position(current_position);
    if (!change) {
      base_position = current_position;
    } else if (current_position < change->new_end) {
      TextSlice slice = TextSlice(*change->new_text).slice({
        Point::min(change->new_end, current_position).traversal(change->new_start),
        goal_position.traversal(change->new_start)
      });
      if (callback(slice)) return true;
      base_position = change->old_end;
      current_position = change->new_end;
    } else {
      base_position = change->old_end.traverse(current_position.traversal(change->new_end));
    }

    auto changes = splay ?
      patch.grab_changes_in_new_range(current_position, goal_position) :
      patch.get_changes_in_new_range(current_position, goal_position);
    for (const auto &change : changes) {
      if (base_position < change.old_start) {
        if (previous_layer->for_each_chunk_in_range(base_position, change.old_start, callback)) {
          return true;
        }
      }

      TextSlice slice = TextSlice(*change.new_text)
        .prefix(Point::min(change.new_end, goal_position).traversal(change.new_start));
      if (callback(slice)) return true;

      base_position = change.old_end;
      current_position = change.new_end;
    }

    if (current_position < goal_position) {
      return previous_layer->for_each_chunk_in_range(
        base_position,
        base_position.traverse(goal_position.traversal(current_position)),
        callback
      );
    }

    return false;
  }

  Point position_for_offset(uint32_t goal_offset) const {
    if (text) {
      return text->position_for_offset(goal_offset);
    } else {
      return patch.new_position_for_new_offset(
        goal_offset,
        [this](Point old_position) {
          return previous_layer->clip_position(old_position).offset;
        },
        [this](uint32_t old_offset) {
          return previous_layer->position_for_offset(old_offset);
        }
      );
    }
  }

  Point extent() const { return extent_; }

  uint32_t size() const { return size_; }

  String text_in_range(Range range, bool splay = false) {
    String result;
    for_each_chunk_in_range(range.start, range.end, [&result](TextSlice slice) {
      result.insert(result.end(), slice.begin(), slice.end());
      return false;
    }, splay);
    return result;
  }

  vector<TextSlice> chunks_in_range(Range range) {
    vector<TextSlice> result;
    for_each_chunk_in_range(range.start, range.end, [&result](TextSlice slice) {
      result.push_back(slice);
      return false;
    });
    return result;
  }

  template <typename Callback>
  void scan_in_range(const Regex &regex, Range range, const Callback &callback, bool splay = false) {
    Regex::MatchData match_data(regex);

    uint32_t minimum_match_row = 0;
    optional<Range> result;
    Text chunk_continuation;
    TextSlice slice_to_search;
    Point chunk_start_position = range.start;
    Point last_search_end_position = range.start;
    Point slice_to_search_start_position = range.start;

    for_each_chunk_in_range(range.start, range.end, [&](TextSlice chunk) {
      Point chunk_end_position = chunk_start_position.traverse(chunk.extent());
      while (last_search_end_position < chunk_end_position) {
        TextSlice remaining_chunk = chunk
          .suffix(last_search_end_position.traversal(chunk_start_position));

        // Once a result is found, we only continue if the match ends with a CR
        // at a chunk boundary. If this chunk starts with an LF, we decrement
        // the column because Points within CRLF line endings are not valid.
        if (result) {
          if (!remaining_chunk.empty() && remaining_chunk.front() == '\n') {
            chunk_continuation.splice(Point(), Point(), Text{u"\r"});
            slice_to_search_start_position.column--;
            result->end.column--;
          }

          if (callback(*result)) return true;
          result = optional<Range>{};
        }

        if (!chunk_continuation.empty()) {
          chunk_continuation.append(remaining_chunk.prefix(MAX_CHUNK_SIZE_TO_COPY));
          slice_to_search = TextSlice(chunk_continuation);
        } else {
          slice_to_search = remaining_chunk;
        }

        MatchResult match_result = regex.match(
          slice_to_search.data(),
          slice_to_search.size(),
          match_data,
          slice_to_search_start_position.traverse(slice_to_search.extent()) == range.end
        );

        switch (match_result.type) {
          case MatchResult::Error:
            chunk_continuation.clear();
            return true;

          case MatchResult::None:
            last_search_end_position = slice_to_search_start_position.traverse(slice_to_search.extent());
            slice_to_search_start_position = last_search_end_position;
            minimum_match_row = slice_to_search_start_position.row;
            chunk_continuation.clear();
            break;

          case MatchResult::Partial:
            last_search_end_position = slice_to_search_start_position.traverse(slice_to_search.extent());
            if (chunk_continuation.empty() || match_result.start_offset > 0) {
              Point partial_match_position = slice_to_search.position_for_offset(match_result.start_offset,
                minimum_match_row - slice_to_search_start_position.row
              );
              slice_to_search_start_position = slice_to_search_start_position.traverse(partial_match_position);
              minimum_match_row = slice_to_search_start_position.row;
              chunk_continuation.assign(slice_to_search.suffix(partial_match_position));
            }
            break;

          case MatchResult::Full:
            Point match_start_position = slice_to_search.position_for_offset(
              match_result.start_offset,
              minimum_match_row - slice_to_search_start_position.row
            );
            Point match_end_position = slice_to_search.position_for_offset(match_result.end_offset,
              minimum_match_row - slice_to_search_start_position.row
            );
            result = Range{
              slice_to_search_start_position.traverse(match_start_position),
              slice_to_search_start_position.traverse(match_end_position)
            };

            minimum_match_row = result->end.row;
            last_search_end_position = slice_to_search_start_position.traverse(match_end_position);
            slice_to_search_start_position = last_search_end_position;
            chunk_continuation.clear();

            // If the match ends with a CR at the end of a chunk, continue looking
            // at the next chunk, in case that chunk starts with an LF. Points
            // within CRLF line endings are not valid.
            if (match_result.end_offset == slice_to_search.size() && slice_to_search.back() == '\r') continue;

            if (callback(*result)) return true;
            result = optional<Range>{};
        }
      }

      chunk_start_position = chunk_end_position;
      return false;
    }, splay);

    if (result) {
      callback(*result);
    } else {
      static uint16_t EMPTY[] = {0};
      MatchResult match_result = regex.match(EMPTY, 0, match_data, true);
      if (match_result.type == MatchResult::Partial || match_result.type == MatchResult::Full) {
        callback(Range{Point(), Point()});
      }
    }
  }

  optional<Range> search_in_range(const Regex &regex, Range range, bool splay = false) {
    optional<Range> result;
    scan_in_range(regex, range, [&result](Range match_range) -> bool {
      result = match_range;
      return true;
    }, splay);
    return result;
  }

  vector<Range> search_all_in_range(const Regex &regex, Range range, bool splay = false) {
    vector<Range> result;
    scan_in_range(regex, range, [&result](Range match_range) -> bool {
      result.push_back(match_range);
      return false;
    }, splay);
    return result;
  }

  bool is_modified(const Layer *base_layer) {
    if (size() != base_layer->size()) return true;

    bool result = false;
    uint32_t start_offset = 0;
    for_each_chunk_in_range(Point(), extent(), [&](TextSlice chunk) {
      if (chunk.text == &(*base_layer->text) ||
          equal(chunk.begin(), chunk.end(), base_layer->text->begin() + start_offset)) {
        start_offset += chunk.size();
        return false;
      }
      result = true;
      return true;
    });

    return result;
  }
};

TextBuffer::TextBuffer(String &&text) :
  base_layer{new Layer(move(text))},
  top_layer{base_layer} {}

TextBuffer::TextBuffer() :
  base_layer{new Layer(Text{})},
  top_layer{base_layer} {}

TextBuffer::~TextBuffer() {
  Layer *layer = top_layer;
  while (layer) {
    Layer *previous_layer = layer->previous_layer;
    delete layer;
    layer = previous_layer;
  }
}

TextBuffer::TextBuffer(const std::u16string &text) :
  TextBuffer{String{text.begin(), text.end()}} {}

void TextBuffer::reset(Text &&new_base_text) {
  if (!top_layer->previous_layer && top_layer->snapshot_count == 0) {
    top_layer->extent_ = new_base_text.extent();
    top_layer->size_ = new_base_text.size();
    top_layer->text = move(new_base_text);
    top_layer->patch.clear();
    top_layer->uses_patch = false;
  } else {
    set_text(String(new_base_text.content));
    flush_changes();
  }
}

Patch TextBuffer::get_inverted_changes(const Snapshot *snapshot) const {
  vector<const Patch *> patches;
  Layer *layer = top_layer;
  while (layer != &snapshot->base_layer) {
    patches.insert(patches.begin(), &layer->patch);
    layer = layer->previous_layer;
  }
  Patch combination(patches);
  TextSlice base{*snapshot->base_layer.text};
  Patch result;
  for (auto change : combination.get_changes()) {
    result.splice(
      change.old_start,
      change.new_end.traversal(change.new_start),
      change.old_end.traversal(change.old_start),
      *change.new_text,
      Text{base.slice({change.old_start, change.old_end})},
      change.new_text->size()
    );
  }
  return result;
}

void TextBuffer::serialize_changes(Serializer &serializer) {
  serializer.append(top_layer->size_);
  top_layer->extent_.serialize(serializer);
  if (top_layer == base_layer) {
    Patch().serialize(serializer);
    return;
  }

  if (top_layer->previous_layer == base_layer) {
    top_layer->patch.serialize(serializer);
    return;
  }

  vector<const Patch *> patches;
  Layer *layer = top_layer;
  while (layer != base_layer) {
    patches.insert(patches.begin(), &layer->patch);
    layer = layer->previous_layer;
  }
  Patch(patches).serialize(serializer);
}

bool TextBuffer::deserialize_changes(Deserializer &deserializer) {
  if (top_layer != base_layer || base_layer->previous_layer) return false;
  top_layer = new Layer(base_layer);
  top_layer->size_ = deserializer.read<uint32_t>();
  top_layer->extent_ = Point(deserializer);
  top_layer->patch = Patch(deserializer);
  return true;
}

const Text &TextBuffer::base_text() const {
  return *base_layer->text;
}

Point TextBuffer::extent() const {
  return top_layer->extent();
}

uint32_t TextBuffer::size() const {
  return top_layer->size();
}

optional<uint32_t> TextBuffer::line_length_for_row(uint32_t row) {
  if (row > extent().row) return optional<uint32_t>{};
  return top_layer->clip_position(Point{row, UINT32_MAX}, true).position.column;
}

const uint16_t *TextBuffer::line_ending_for_row(uint32_t row) {
  if (row > extent().row) return nullptr;

  static uint16_t LF[] = {'\n', 0};
  static uint16_t CRLF[] = {'\r', '\n', 0};
  static uint16_t NONE[] = {0};

  const uint16_t *result = NONE;
  top_layer->for_each_chunk_in_range(
    Point(row, UINT32_MAX),
    Point(row + 1, 0),
    [&result](TextSlice slice) {
      auto begin = slice.begin();
      if (begin == slice.end()) return false;
      result = (*begin == '\r') ? CRLF : LF;
      return true;
    }, true);
  return result;
}

void TextBuffer::with_line_for_row(uint32_t row, const std::function<void(const uint16_t *, uint32_t)> &callback) {
  Text::String result;
  uint32_t column = 0;
  uint32_t slice_count = 0;
  Point line_end = clip_position({row, UINT32_MAX}).position;
  top_layer->for_each_chunk_in_range({row, 0}, line_end, [&](TextSlice slice) -> bool {
    auto begin = slice.begin(), end = slice.end();
    size_t size = end - begin;
    slice_count++;
    column += size;
    if (slice_count == 1 && column == line_end.column) {
      callback(slice.data(), slice.size());
      return true;
    } else {
      result.insert(result.end(), begin, end);
      return false;
    }
  });

  if (slice_count != 1) {
    callback(result.data(), result.size());
  }
}

optional<String> TextBuffer::line_for_row(uint32_t row) {
  if (row > extent().row) return optional<String>{};
  return text_in_range({{row, 0}, {row, UINT32_MAX}});
}

ClipResult TextBuffer::clip_position(Point position) {
  return top_layer->clip_position(position, true);
}

Point TextBuffer::position_for_offset(uint32_t offset) {
  return top_layer->position_for_offset(offset);
}

String TextBuffer::text() {
  return top_layer->text_in_range(Range{Point(), extent()});
}

String TextBuffer::text_in_range(Range range) {
  return top_layer->text_in_range(range, true);
}

vector<TextSlice> TextBuffer::chunks() const {
  return top_layer->chunks_in_range({{0, 0}, extent()});
}

void TextBuffer::set_text(String &&new_text) {
  set_text_in_range(Range{Point(0, 0), extent()}, move(new_text));
}

void TextBuffer::set_text(const u16string &string) {
  set_text(String(string.begin(), string.end()));
}

void TextBuffer::set_text_in_range(Range old_range, String &&string) {
  if (top_layer == base_layer || top_layer->snapshot_count > 0) {
    top_layer = new Layer(top_layer);
  }

  auto start = clip_position(old_range.start);
  auto end = clip_position(old_range.end);
  Point deleted_extent = end.position.traversal(start.position);
  Text new_text{move(string)};
  Point inserted_extent = new_text.extent();
  Point new_range_end = start.position.traverse(new_text.extent());
  uint32_t deleted_text_size = end.offset - start.offset;
  top_layer->extent_ = new_range_end.traverse(top_layer->extent_.traversal(end.position));
  top_layer->size_ += new_text.size() - deleted_text_size;
  top_layer->patch.splice(
    start.position,
    deleted_extent,
    inserted_extent,
    optional<Text>{},
    move(new_text),
    deleted_text_size
  );

  auto change = top_layer->patch.grab_change_starting_before_new_position(start.position);
  if (change && change->old_text_size == change->new_text->size()) {
    bool change_is_noop = true;
    auto new_text_iter = change->new_text->begin();
    top_layer->previous_layer->for_each_chunk_in_range(
      change->old_start,
      change->old_end,
      [&change_is_noop, &new_text_iter](TextSlice chunk) {
        auto new_text_end = new_text_iter + chunk.size();
        if (!std::equal(new_text_iter, new_text_end, chunk.begin())) {
          change_is_noop = false;
          return true;
        }
        new_text_iter = new_text_end;
        return false;
      });
    if (change_is_noop) {
      top_layer->patch.splice_old(change->old_start, Point(), Point());
    }
  }
}

void TextBuffer::set_text_in_range(Range old_range, const u16string &string) {
  set_text_in_range(old_range, String(string.begin(), string.end()));
}

optional<Range> TextBuffer::search(const Regex &regex) const {
  return top_layer->search_in_range(regex, Range{Point(), extent()}, false);
}

vector<Range> TextBuffer::search_all(const Regex &regex) const {
  return top_layer->search_all_in_range(regex, Range{Point(), extent()}, false);
}

bool TextBuffer::is_modified() const {
  return top_layer->is_modified(base_layer);
}

bool TextBuffer::is_modified(const Snapshot *snapshot) const {
  return top_layer->is_modified(&snapshot->base_layer);
}

string TextBuffer::get_dot_graph() const {
  Layer *layer = top_layer;
  vector<Layer *> layers;
  while (layer) {
    layers.push_back(layer);
    layer = layer->previous_layer;
  }

  std::stringstream result;
  result << "graph { label=\"--- buffer ---\" }\n";
  for (auto begin = layers.rbegin(), iter = begin, end = layers.rend();
       iter != end; ++iter) {
    auto layer = *iter;
    auto index = iter - begin;
    result << "graph { label=\"layer " << index << " (snapshot count " << layer->snapshot_count;
    if (layer == base_layer) result << ", base";
    if (layer->uses_patch) result << ", uses_patch";
    result << "):\" }\n";
    if (layer->text) result << "graph { label=\"text:\n" << *layer->text << "\" }\n";
    if (index > 0) result << layer->patch.get_dot_graph();
  }
  return result.str();
}

size_t TextBuffer::layer_count() const {
  size_t result = 1;
  const Layer *layer = top_layer;
  while (layer->previous_layer) {
    result++;
    layer = layer->previous_layer;
  }
  return result;
}

TextBuffer::Snapshot *TextBuffer::create_snapshot() {
  top_layer->snapshot_count++;
  base_layer->snapshot_count++;
  return new Snapshot(*this, *top_layer, *base_layer);
}

void TextBuffer::flush_changes() {
  if (!top_layer->text) {
    top_layer->text = Text{text()};
    base_layer = top_layer;
    consolidate_layers();
  }
}

uint32_t TextBuffer::Snapshot::size() const {
  return layer.size();
}

Point TextBuffer::Snapshot::extent() const {
  return layer.extent();
}

uint32_t TextBuffer::Snapshot::line_length_for_row(uint32_t row) const {
  return layer.clip_position(Point{row, UINT32_MAX}).position.column;
}

String TextBuffer::Snapshot::text_in_range(Range range) const {
  return layer.text_in_range(range);
}

String TextBuffer::Snapshot::text() const {
  return layer.text_in_range({{0, 0}, extent()});
}

vector<TextSlice> TextBuffer::Snapshot::chunks_in_range(Range range) const {
  return layer.chunks_in_range(range);
}

vector<TextSlice> TextBuffer::Snapshot::chunks() const {
  return layer.chunks_in_range({{0, 0}, extent()});
}

optional<Range> TextBuffer::Snapshot::search(const Regex &regex) const {
  return layer.search_in_range(regex, Range{Point(), extent()}, false);
}

const Text &TextBuffer::Snapshot::base_text() const {
  return *base_layer.text;
}

TextBuffer::Snapshot::Snapshot(TextBuffer &buffer, TextBuffer::Layer &layer,
                               TextBuffer::Layer &base_layer)
  : buffer{buffer}, layer{layer}, base_layer{base_layer} {}

void TextBuffer::Snapshot::flush_preceding_changes() {
  if (!layer.text) {
    layer.text = Text{text()};
    if (layer.is_above_layer(buffer.base_layer)) buffer.base_layer = &layer;
    buffer.consolidate_layers();
  }
}

TextBuffer::Snapshot::~Snapshot() {
  assert(layer.snapshot_count > 0);
  layer.snapshot_count--;
  base_layer.snapshot_count--;
  if (layer.snapshot_count == 0 || base_layer.snapshot_count == 0) {
    buffer.consolidate_layers();
  }
}

void TextBuffer::consolidate_layers() {
  Layer *layer = top_layer;
  vector<Layer *> mutable_layers;
  bool needed_by_layer_above = false;

  while (layer) {
    if (needed_by_layer_above || layer->snapshot_count > 0) {
      squash_layers(mutable_layers);
      mutable_layers.clear();
      needed_by_layer_above = true;
    } else {
      if (layer == base_layer) {
        squash_layers(mutable_layers);
        mutable_layers.clear();
      }

      if (layer->text) layer->uses_patch = false;
      mutable_layers.push_back(layer);
    }

    if (!layer->uses_patch) needed_by_layer_above = false;
    layer = layer->previous_layer;
  }

  squash_layers(mutable_layers);
}

void TextBuffer::squash_layers(const vector<Layer *> &layers) {
  size_t layer_index = 0;
  size_t layer_count = layers.size();
  if (layer_count < 2) return;

  // Find the highest layer that has already computed its text.
  optional<Text> text;
  for (layer_index = 0; layer_index < layer_count; layer_index++) {
    if (layers[layer_index]->text) {
      text = move(*layers[layer_index]->text);
      break;
    }
  }

  // Incorporate into that text the patches from all the layers above.
  if (text) {
    layer_index--;
    for (; layer_index + 1 > 0; layer_index--) {
      for (auto change : layers[layer_index]->patch.get_changes()) {
        text->splice(
          change.new_start,
          change.old_end.traversal(change.old_start),
          *change.new_text
        );
      }
    }
  }

  // If there is another layer below these layers, combine their patches into
  // into one. Otherwise, this is the new base layer, so we don't need a patch.
  Patch patch;
  Layer *previous_layer = layers.back()->previous_layer;

  if (previous_layer) {
    layer_index = layer_count - 1;
    patch = move(layers[layer_index]->patch);
    layer_index--;

    bool left_to_right = true;
    for (; layer_index + 1 > 0; layer_index--) {
      patch.combine(layers[layer_index]->patch, left_to_right);
      left_to_right = !left_to_right;
    }
  } else {
    assert(text);
  }

  layers[0]->previous_layer = previous_layer;
  layers[0]->text = move(text);
  layers[0]->patch = move(patch);

  for (layer_index = 1; layer_index < layer_count; layer_index++) {
    delete layers[layer_index];
  }
}
