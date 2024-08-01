#ifndef LUCENE_CYBORG_CPP_SRC_CODECS_LUCENE90_BLOCK_TREE_SEGMENTTERMSENUMFRAME_HPP_
#define LUCENE_CYBORG_CPP_SRC_CODECS_LUCENE90_BLOCK_TREE_SEGMENTTERMSENUMFRAME_HPP_

#include <optional>
#include <string_view>
#include "CompressionAlgorithm.hpp"
#include "../../../store/DataInput.hpp"
#include "../../../store/ReadOnlyDataInput.hpp"
#include "../../../store/MeasureDataInput.hpp"
#include "../../../util/LinearAllocator.hpp"
#include "../../../index/TermsEnum.hpp"
#include "../../../index/IndexOptions.hpp"
#include "../../../index/FieldInfo.hpp"

namespace lucene::cyborg::codecs::v90::block_tree {

struct SegmentTermsEnumFrame {
  static std::optional<int32_t> measure_block_size(store::MeasureDataInput &input);

  SegmentTermsEnumFrame(util::UseOnlyPtr<uint8_t> frame_code,
                        int32_t _frame_code_len,
                        util::LinearAllocator *allocator,
                        store::DataInput<store::ReadOnlyDataInput> terms_in);

  SegmentTermsEnumFrame(int64_t block_fp,
                        util::LinearAllocator *allocator,
                        store::DataInput<store::ReadOnlyDataInput> terms_in);

  void scan_to_floor_frame(const uint8_t label);

  void load_block();

  index::TermsEnum::SeekStatus scan_to_term(const std::string_view &term_suffix, bool exact_only);

  void set_floor_data(util::UseOnlyPtr<uint8_t> frame_code, int32_t len);

  index::TermsEnum::SeekStatus scan_to_term_leaf(const std::string_view &term_suffix, bool exact_only);

  index::TermsEnum::SeekStatus scan_to_term_non_leaf(const std::string_view &term_suffix, bool exact_only);

  void load_next_floor_block();

  bool next_entry();

  void reset_for_traversal(int64_t block_fp);

  template<class PostingsReader>
  void decode_meta_data(util::ManagedPtr<PostingsReader> postings_reader,
                        typename PostingsReader::TermState &term_state) {
    const int32_t limit = is_leaf_block ? next_ent : term_block_order;
    auto meta_data_upto = this->meta_data_upto;
    auto stats_singleton_run_length = this->stats_singleton_run_length;

    // Skip redundant bytes
    while (meta_data_upto < limit) {
      if (stats_singleton_run_length > 0) {
        --stats_singleton_run_length;
        term_state.total_term_freq = term_state.doc_freq = 1;
      } else {
        const uint32_t stats_code = stats_bytes_input.read_vint();
        if (stats_code & 1U) {
          term_state.total_term_freq = term_state.doc_freq = 1;
          stats_singleton_run_length = stats_code >> 1U;
        } else {
          term_state.doc_freq = stats_code >> 1U;
          if (term_state.index_options == 1U) {
            term_state.total_term_freq = stats_code >> 1U;
          } else {
            term_state.total_term_freq = (stats_code >> 1U) + stats_bytes_input.read_vlong();
          }
        }
      }  // End if

      postings_reader->decode_term(meta_bytes_input, term_state);
      ++meta_data_upto;
    }  // End while

    this->meta_data_upto = meta_data_upto;
    this->stats_singleton_run_length = stats_singleton_run_length;
  }

  //
  // Common
  //
  util::LinearAllocator *allocator;
  int64_t org_block_fp;
  int64_t block_fp;
  int64_t block_fp_end;

  // Floor blocks : Not used during the traversal, as these are given from FST index.
  store::DataInput<store::ReadOnlyDataInput> floor_data_reader;
  int32_t num_follow_floor_blocks;
  uint32_t next_floor_label;
  bool is_last_in_floor;
  int32_t next_ent;
  store::DataInput<store::ReadOnlyDataInput> terms_in;

  //
  // Block header
  //
  bool has_terms;
  bool is_floor;
  int32_t num_entries;

  //
  // Suffix - Common
  //
  util::ReadOnlyPtr<uint8_t> suffix_bytes_start;
  store::DataInput<store::ReadOnlyDataInput> suffix_lengths_input;
  std::string_view decompression_buffer;
  std::string_view suffix;
  int32_t suffix_length;
  uint32_t suffix_length_if_all_equals_vint;
  uint32_t suffix_length_if_all_equals_vlong;
  CompressionAlgorithmType compression_algorithm_type;
  bool all_equal;
  bool is_leaf_block;

  //
  // Suffix - Non-leaf
  //
  bool is_term_entry;
  int32_t term_block_order;
  int64_t sub_block_fp;

  //
  // Stats
  //
  store::DataInput<store::ReadOnlyDataInput> stats_bytes_input;
  int32_t stats_singleton_run_length;

  //
  // Meta
  //
  store::DataInput<store::ReadOnlyDataInput> meta_bytes_input;
  int32_t meta_data_upto;

 private:
  void next_leaf();

  bool next_non_leaf();
};  // SegmentTermsEnumFrame



}  // lucene::cyborg::codecs

#endif //LUCENE_CYBORG_CPP_SRC_CODECS_LUCENE90_BLOCK_TREE_SEGMENTTERMSENUMFRAME_HPP_
