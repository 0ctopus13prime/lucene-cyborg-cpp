#include "SegmentTermsEnumFrame.hpp"
#include "BlockTreeConstants.hpp"

using lucene::cyborg::codecs::v90::block_tree::SegmentTermsEnumFrame;
using lucene::cyborg::index::TermsEnum;
using lucene::cyborg::store::DataDecoding;
using lucene::cyborg::store::DataInput;
using lucene::cyborg::store::MeasureDataInput;
using lucene::cyborg::store::ReadOnlyDataInput;
using lucene::cyborg::util::LinearAllocator;
using lucene::cyborg::util::MemoryUtils;
using lucene::cyborg::util::UseOnlyPtr;

//
// SegmentTermsEnumFrame
//

std::optional<int32_t> SegmentTermsEnumFrame::measure_block_size(MeasureDataInput &input) {
  // header
  auto header = input.read_vint();
  if (!header) {
    return std::nullopt;
  }

  // terms suffix bytes + suffix lengths
  auto code_long = input.read_vlong();
  if (!code_long) {
    return std::nullopt;
  }
  const auto num_suffix_bytes = (int32_t) ((*code_long) >> 3U);
  if (!input.skip_bytes(num_suffix_bytes)) {
    return std::nullopt;
  }

  auto num_suffix_length_bytes = input.read_vint();
  if (!num_suffix_length_bytes) {
    return std::nullopt;
  }
  const auto all_eq = ((*num_suffix_length_bytes) & 0x01U);
  if (all_eq) {
    if (!input.read_byte()) {
      return std::nullopt;
    }
  } else {
    if (!input.skip_bytes((*num_suffix_length_bytes) >> 1U)) {
      return std::nullopt;
    }
  }

  // stats
  auto num_stats_bytes = input.read_vint();
  if (!num_stats_bytes) {
    return std::nullopt;
  }
  if (!input.skip_bytes(*num_stats_bytes)) {
    return std::nullopt;
  }

  // metadata
  auto metadata_num_bytes = input.read_vint();
  if (!metadata_num_bytes) {
    return std::nullopt;
  }
  if (!input.skip_bytes(*metadata_num_bytes)) {
    return std::nullopt;
  }

  return input.data - input.org_data;
}

SegmentTermsEnumFrame::SegmentTermsEnumFrame(UseOnlyPtr<uint8_t> _frame_code,
                                             const int32_t _frame_code_len,
                                             LinearAllocator *_allocator,
                                             DataInput<ReadOnlyDataInput> _terms_in)
    : allocator(_allocator),
      org_block_fp(),
      block_fp(),
      block_fp_end(),
      floor_data_reader(),
      num_follow_floor_blocks(),
      next_floor_label(),
      is_last_in_floor(),
      next_ent(-1),
      terms_in(_terms_in),
      has_terms(),
      is_floor(),
      num_entries(),
      suffix_bytes_start(),
      suffix_lengths_input(),
      decompression_buffer(),
      suffix(),
      suffix_length(),
      suffix_length_if_all_equals_vint(),
      suffix_length_if_all_equals_vlong(),
      compression_algorithm_type(),
      all_equal(),
      is_leaf_block(),
      is_term_entry(),
      term_block_order(),
      sub_block_fp(),
      stats_bytes_input(),
      stats_singleton_run_length(),
      meta_bytes_input(),
      meta_data_upto() {
  auto org_frame_code = _frame_code;
  const auto code = DataDecoding::read_vlong(_frame_code);
  org_block_fp = block_fp = code >> BlockTreeConstants::OUTPUT_FLAGS_NUM_BITS;
  has_terms = code & BlockTreeConstants::OUTPUT_FLAG_HAS_TERMS;
  is_floor = code & BlockTreeConstants::OUTPUT_FLAG_IS_FLOOR;
  if (is_floor) {
    set_floor_data(_frame_code, _frame_code_len - (uint64_t(_frame_code) - uint64_t(org_frame_code)));
  }
}

SegmentTermsEnumFrame::SegmentTermsEnumFrame(int64_t _block_fp,
                                             LinearAllocator *_allocator,
                                             DataInput<ReadOnlyDataInput> _terms_in)
    : allocator(_allocator),
      org_block_fp(_block_fp),
      block_fp(_block_fp),
      block_fp_end(),
      floor_data_reader(),
      num_follow_floor_blocks(),
      next_floor_label(),
      is_last_in_floor(),
      next_ent(-1),
      terms_in(_terms_in),
      has_terms(),
      is_floor(),
      num_entries(),
      suffix_bytes_start(),
      suffix_lengths_input(),
      decompression_buffer(),
      suffix(),
      suffix_length(),
      suffix_length_if_all_equals_vint(),
      suffix_length_if_all_equals_vlong(),
      compression_algorithm_type(),
      all_equal(),
      is_leaf_block(),
      is_term_entry(),
      term_block_order(),
      sub_block_fp(),
      stats_singleton_run_length(),
      stats_bytes_input(),
      meta_bytes_input(),
      meta_data_upto() {
}

void SegmentTermsEnumFrame::scan_to_floor_frame(const uint8_t label) {
  if (!is_floor) {
    return;
  }

  if (label < next_floor_label) {
    return;
  }

  assert(num_follow_floor_blocks != 0);

  int64_t new_fp;
  while (true) {
    const auto code = floor_data_reader.read_vlong();
    new_fp = org_block_fp + (code >> 1U);
    has_terms = (code & 1U) != 0;
    is_last_in_floor = (num_follow_floor_blocks == 1);
    --num_follow_floor_blocks;

    if (is_last_in_floor) {
      next_floor_label = 256U;
      break;
    } else {
      next_floor_label = floor_data_reader.read_byte() & 0xFFU;
      if (label < next_floor_label) {
        break;
      }
    }
  }  // End while

  if (new_fp != block_fp) {
    // Force re-load of floor block
    next_ent = -1;
    block_fp = new_fp;
  }  // End if
}

void SegmentTermsEnumFrame::load_block() {
  if (next_ent != -1) {
    // Already loaded
    return;
  }

  //
  // Block header
  //
  terms_in.seek(block_fp);
  const uint32_t block_code = terms_in.read_vint();
  num_entries = block_code >> 1U;
  assert(num_entries > 0);
  is_last_in_floor = (block_code & 1U);

  //
  // Suffix bytes
  //
  const auto suffix_bytes_code = terms_in.read_vlong();
  is_leaf_block = suffix_bytes_code & 0x04U;
  const int32_t num_suffix_bytes = (suffix_bytes_code >> 3U);
  const auto compression_alg_code = suffix_bytes_code & 0x03U;
  if (compression_alg_code != 0U) {
    // compressed
    auto compression_alg = CompressionAlgorithm::get_algorithm(compression_alg_code);
    if (compression_alg == nullptr) {
      // TODO
      throw 13;
    }
    compression_algorithm_type = compression_alg->compression_algorithm_type;

    // Start decompression
    const auto decompressed_suffix_bytes_len = MemoryUtils::oversize(num_suffix_bytes);
    // Allocate a decompression buffer if it needs to
    if (decompressed_suffix_bytes_len > decompression_buffer.size()) {
      decompression_buffer = std::string_view{
          (char *) allocator->allocate(decompressed_suffix_bytes_len),
          (uint64_t) decompressed_suffix_bytes_len
      };
    }

    // Trigger compression logic
    compression_alg
        ->read(terms_in,
               (char *) decompression_buffer.data(),
               num_suffix_bytes);
    suffix_bytes_start = (uint8_t *) decompression_buffer.data();
  } else {
    compression_algorithm_type = CompressionAlgorithmType::NO_COMPRESSION;
    suffix_bytes_start = terms_in.input.data;
    terms_in.skip_bytes(num_suffix_bytes);
  }  // End if


  //
  // Suffix lengths
  //
  const uint32_t suffix_lengths_code = terms_in.read_vint();
  const int32_t num_suffix_length_bytes = suffix_lengths_code >> 1U;
  all_equal = (suffix_lengths_code & 0x01U);
  if (all_equal) {
    const uint8_t length_first_byte = terms_in.read_byte();
    const uint8_t temp_bytes[9] = {length_first_byte, length_first_byte, length_first_byte,
                                   length_first_byte, length_first_byte, length_first_byte,
                                   length_first_byte, length_first_byte, length_first_byte};
    const uint8_t *temp_bytes_ptr = &temp_bytes[0];
    suffix_length_if_all_equals_vint = DataDecoding::read_vint(temp_bytes_ptr);
    temp_bytes_ptr = &temp_bytes[0];
    suffix_length_if_all_equals_vlong = DataDecoding::read_vlong(temp_bytes_ptr);
  } else {
    suffix_lengths_input = DataInput<ReadOnlyDataInput>{
        ReadOnlyDataInput{
            terms_in.input.data,
            terms_in.input.data,
            0,
            num_suffix_length_bytes
        }
    };
    terms_in.skip_bytes(num_suffix_length_bytes);
  }  // End if

  //
  // Stats
  //
  const int32_t num_stat_bytes = terms_in.read_vint();
  stats_bytes_input = DataInput<ReadOnlyDataInput>{
      ReadOnlyDataInput{
          terms_in.input.data,
          terms_in.input.data,
          0,
          num_stat_bytes
      }
  };
  terms_in.skip_bytes(num_stat_bytes);

  next_ent = 0;

  //
  // Meta
  //
  const int32_t num_meta_bytes = terms_in.read_vint();
  meta_bytes_input = DataInput<ReadOnlyDataInput>{
      ReadOnlyDataInput{
          terms_in.input.data,
          terms_in.input.data,
          0,
          num_stat_bytes
      }
  };
  terms_in.skip_bytes(num_meta_bytes);
  block_fp_end = terms_in.get_file_pointer();
}

TermsEnum::SeekStatus SegmentTermsEnumFrame::scan_to_term(
    const std::string_view &term_suffix,
    bool exact_only) {
  return is_leaf_block ? scan_to_term_leaf(term_suffix, exact_only)
                       : scan_to_term_non_leaf(term_suffix, exact_only);
}

void SegmentTermsEnumFrame::set_floor_data(UseOnlyPtr<uint8_t> frame_code, int32_t len) {
  floor_data_reader = DataInput<ReadOnlyDataInput>{
      ReadOnlyDataInput{
          frame_code,
          frame_code,
          0,
          len
      }
  };
  num_follow_floor_blocks = floor_data_reader.read_vint();
  next_floor_label = floor_data_reader.read_byte();
}

TermsEnum::SeekStatus SegmentTermsEnumFrame::scan_to_term_leaf(const std::string_view &term_suffix,
                                                               bool exact_only) {
  assert(next_ent != -1);

  if (next_ent == num_entries) {
    return TermsEnum::SeekStatus::END;
  }

  auto curr_suffix_bytes = suffix_bytes_start;

  do {
    ++next_ent;
    suffix_length = all_equal ?
                    suffix_length_if_all_equals_vint : suffix_lengths_input.read_vint();
    suffix = std::string_view{(char *) curr_suffix_bytes, (uint64_t) suffix_length};
    curr_suffix_bytes += suffix_length;

    // Loop over bytes in the suffix, comparing to the target
    const auto cmp = suffix.compare(term_suffix);

    if (cmp < 0) {
      // Current entry is still before the target;
      // keep scanning
    } else if (cmp > 0) {
      // Done!  Current entry is after target --
      return TermsEnum::SeekStatus::NOT_FOUND;
    } else {
      // Exact match!

      // This cannot be a sub-block because we
      // would have followed the index to this
      // sub-block from the start:
      return TermsEnum::SeekStatus::FOUND;
    }  // End if
  } while (next_ent < num_entries);

  // It is possible (and OK) that terms index pointed us
  // at this block, but, we scanned the entire block and
  // did not find the term to position to.
  // This happens when the target is after the last term in the block
  // (but, before the next term in the index).
  // EG target could be foozzz, and terms index pointed us
  // to the foo* block, but the last term in this block
  // was fooz (and, eg, first term in the next block will
  // bee fop).
  return TermsEnum::SeekStatus::END;
}

TermsEnum::SeekStatus SegmentTermsEnumFrame::scan_to_term_non_leaf(const std::string_view &term_suffix,
                                                                   bool exact_only) {
  assert(next_ent != -1);

  if (next_ent == num_entries) {
    return TermsEnum::SeekStatus::END;
  }

  auto curr_suffix_bytes = suffix_bytes_start;

  // TODO : KDY
  // can we just search the suffix in the suffix block?
  // this can be much faster due to the cache look up, will need a benchmark test though
  // After suffix match, we try to verify whether we matched a suffix using length information.
  // ex:
  //    idx = 0
  //    SEARCH_LABEL:
  //    pos = find(suffix_bytes, suffix, idx)
  //    if (pos == -1) return false
  //    while ((idx += read_vint()) < pos);
  //    if (idx == pos) return true;
  //    ++idx
  //    goto SEARCH_LABEL

  // Loop over each entry (term or sub-block) in this block:
  while (next_ent < num_entries) {
    ++next_ent;
    const uint32_t suffix_length_code = all_equal ?
                                        suffix_length_if_all_equals_vint : suffix_lengths_input.read_vint();
    is_term_entry = (suffix_length_code & 1U) == 0;
    suffix_length = suffix_length_code >> 1U;

    if (is_term_entry) {
      ++term_block_order;
      suffix = std::string_view((char *) curr_suffix_bytes, (uint64_t) suffix_length);
      curr_suffix_bytes += suffix_length;

      const auto cmp = suffix.compare(term_suffix);
      if (cmp < 0) {
        // Current entry is still before the target;
        // keep scanning
      } else if (cmp > 0) {
        // Done!  Current entry is after target
        return TermsEnum::SeekStatus::NOT_FOUND;
      } else {
        // Exact match!
        return TermsEnum::SeekStatus::FOUND;
      }  // End if
    } else {
      // exhaust sub block fp
      if (!all_equal) {
        suffix_lengths_input.read_vlong();
      }
      curr_suffix_bytes += suffix_length;
    }  // End if
  }  // End while

  // It is possible (and OK) that terms index pointed us
  // at this block, but, we scanned the entire block and
  // did not find the term to position to.
  // This happens when the target is after the last term in the block
  // (but, before the next term in the index).
  // EG target could be foozzz, and terms index pointed us
  // to the foo* block, but the last term in this block
  // was fooz (and, eg, first term in the next block will
  // bee fop).
  return TermsEnum::SeekStatus::END;
}

void SegmentTermsEnumFrame::load_next_floor_block() {
  block_fp = block_fp_end;
  next_ent = -1;
  load_block();
}

bool SegmentTermsEnumFrame::next_entry() {
  if (is_leaf_block) {
    next_leaf();
    return false;
  } else {
    return next_non_leaf();
  }
}

void SegmentTermsEnumFrame::next_leaf() {
  ++next_ent;
  ++term_block_order;
  suffix_length = all_equal ?
                  suffix_length_if_all_equals_vint : suffix_lengths_input.read_vint();
  suffix = std::string_view((char *) suffix_bytes_start, (uint64_t) suffix_length);
  suffix_bytes_start += suffix_length;
}

bool SegmentTermsEnumFrame::next_non_leaf() {
  while (true) {
    if (next_ent == num_entries) {
      assert(is_floor && !is_last_in_floor);
      load_next_floor_block();
      if (is_leaf_block) {
        next_leaf();
        return false;
      } else {
        continue;
      }
    }

    ++next_ent;
    const uint32_t code = all_equal ?
                          suffix_length_if_all_equals_vint : suffix_lengths_input.read_vint();
    suffix_length = code >> 1U;
    suffix = std::string_view((char *) suffix_bytes_start, (uint64_t) suffix_length);
    suffix_bytes_start += suffix_length;

    if (code & 1U) {
      // A sub-block; make sub-FP absolute:
      const uint64_t file_pointer_delta = all_equal ?
                                          suffix_length_if_all_equals_vlong : suffix_lengths_input.read_vlong();
      sub_block_fp = block_fp - file_pointer_delta;
      return true;
    } else {
      ++term_block_order;
      // Normal term
      return false;
    }
  }  // End while
}

void SegmentTermsEnumFrame::reset_for_traversal(int64_t _block_fp) {
  org_block_fp = block_fp = _block_fp;
  terms_in.seek(_block_fp);
  next_ent = -1;
  term_block_order = 0;
}
