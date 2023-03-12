/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "h264_common.h"

#include <cstdint>

// namespace webrtc {
namespace H264 {

const uint8_t kNaluTypeMask = 0x1F;
std::vector<NaluIndex> nalu_indices;
std::vector<NaluIndex>* FindNaluIndices(const uint8_t* buffer,
                                       size_t buffer_size) {
  // This is sorta like Boyer-Moore, but with only the first optimization step:
  // given a 3-byte sequence we're looking at, if the 3rd byte isn't 1 or 0,
  // skip ahead to the next 3-byte sequence. 0s and 1s are relatively rare, so
  // this will skip the majority of reads/checks.
  if (buffer_size < kNaluShortStartSequenceSize) {
    return nalu_indices;
  }

  nalu_indices.clear();
  static_assert(kNaluShortStartSequenceSize >= 2,
                "kNaluShortStartSequenceSize must be larger or equals to 2");
  const size_t end = buffer_size - kNaluShortStartSequenceSize;
  for (size_t i = 0; i < end;) {
    if (buffer[i + 2] > 1) {
      i += 3;
    } else if (buffer[i + 2] == 1) {
      if (buffer[i + 1] == 0 && buffer[i] == 0) {
        // We found a start sequence, now check if it was a 3 of 4 byte one.
        NaluIndex index = {i, i + 3, 0};
        if (index.start_offset > 0 && buffer[index.start_offset - 1] == 0)
          --index.start_offset;

        // Update length of previous entry.
        auto it = nalu_indices.rbegin();
        if (it != nalu_indices.rend())
          it->payload_size = index.start_offset - it->payload_start_offset;

        nalu_indices.push_back(index);
      }

      i += 3;
    } else {
      ++i;
    }
  }

  // Update length of last entry, if any.
  auto it = nalu_indices.rbegin();
  if (it != nalu_indices.rend())
    it->payload_size = buffer_size - it->payload_start_offset;

  return &nalu_indices;
}

NaluType ParseNaluType(uint8_t data) {
  return static_cast<NaluType>(data & kNaluTypeMask);
}
}  // namespace H264
// }  // namespace webrtc
