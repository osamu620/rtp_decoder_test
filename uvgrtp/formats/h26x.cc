#include "h26x.hh"

#include "../socket.hh"

#include "../rtp.hh"
#include "../frame_queue.hh"
#include "../debug.hh"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <queue>
#include <algorithm>

#ifdef _WIN32
  #include <ws2def.h>
  #include <ws2ipdef.h>
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#define PTR_DIFF(a, b) ((ptrdiff_t)((char*)(a) - (char*)(b)))

// see https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord
#define haszero64_le(v) (((v) - 0x0101010101010101) & ~(v) & 0x8080808080808080UL)
#define haszero32_le(v) (((v) - 0x01010101) & ~(v) & 0x80808080UL)

#define haszero64_be(v) (((v) - 0x1010101010101010) & ~(v) & 0x0808080808080808UL)
#define haszero32_be(v) (((v) - 0x10101010) & ~(v) & 0x08080808UL)

#ifndef __LITTLE_ENDIAN
  #define __LITTLE_ENDIAN 1337
#endif

#ifndef __BYTE_ORDER
  #define __BYTE_ORDER __LITTLE_ENDIAN
#endif

constexpr int GARBAGE_COLLECTION_INTERVAL_MS = 100;

// any value less than 30 minutes is ok here, since that is how long it takes to go through all timestamps
[[maybe_unused]] constexpr int TIME_TO_KEEP_TRACK_OF_PREVIOUS_FRAMES_MS = 5000;

// how many RTP timestamps get saved for duplicate detection
// there is 90 000 timestamps in one second -> 5 sec is 450 000
constexpr int RECEIVED_FRAMES = 450000;

static inline uint8_t determine_start_prefix_precense(uint32_t value, bool& additional_byte) {
  additional_byte = false;
#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint16_t cur_ls = (value >> 16) & 0xffff;
  uint16_t cur_ms = (value >> 0) & 0xffff;

  // zeros in more significant bytes
  bool ms4z = (cur_ms == 0);
  bool ms2z = (((cur_ms >> 8) & 0xff) == 0);

  // possible start code end in less significant bytes
  bool ls2s = ((cur_ls & 0xff) == 0x01);
  bool ls4s = (cur_ls == 0x0100);

#else
  uint16_t cur_ls = (value >> 0) & 0xffff;
  uint16_t cur_ms = (value >> 16) & 0xffff;

  bool ms4z = (cur_ms == 0);
  bool ms2z = ((cur_ms & 0xff) == 0);
  bool ls2s = (((cur_ls >> 8) & 0xff) == 0x01);
  bool ls4s = (cur_ls == 0x0001);

#endif

  if (ms4z) {
    /* 0x00000001 */
    if (ls4s) return 4;

    /* "value" definitely has a start code (0x000001XX), but at this
     * point we can't know for sure whether it's 3 or 4 bytes long.
     *
     * Return 5 to indicate that start length could not be determined
     * and that caller must check previous dword's last byte for 0x00 */
    if (ls2s) return 5;
  } else if (ms2z && ls4s) {
    /* 0xXX000001 */
    additional_byte = true;
    return 4;
  }

  return 0;
}

uvgrtp::formats::h26x::h26x(std::shared_ptr<uvgrtp::socket> socket, std::shared_ptr<uvgrtp::rtp> rtp,
                            int rce_flags)
    : media(socket, rtp, rce_flags),
      queued_(),
      access_units_(),
      received_frames_(),
      received_info_(),
      fragments_(),
      dropped_ts_(),
      dropped_in_order_(),
      rtp_ctx_(rtp),
      last_garbage_collection_(uvgrtp::clock::hrc::now()),
      discard_until_key_frame_(true) {}

uvgrtp::formats::h26x::~h26x() {
  for (auto& frame : queued_) {
    (void)uvgrtp::frame::dealloc_frame(frame);
  }

  queued_.clear();

  for (auto& fragment : fragments_) {
    if (fragment.second != nullptr) {
      (void)uvgrtp::frame::dealloc_frame(fragment.second);
    }
  }

  fragments_.clear();
}

/* NOTE: the area 0 - len (ie data[0] - data[len - 1]) must be addressable
 * Do not add offset to "data" ptr before passing it to find_h26x_start_code()! */
ssize_t uvgrtp::formats::h26x::find_h26x_start_code(uint8_t* data, size_t len, size_t offset,
                                                    uint8_t& start_len) {
  if (data == nullptr || len < offset || len < 1) {
    UVG_LOG_WARN("Invalid parameter found for start code lookup");
    return -1;
  }

  bool prev_had_zero = false;
  bool cur_has_zero  = false;
  size_t pos         = offset;

  uint32_t prev_value32 = UINT32_MAX;
  uint32_t cur_value32  = UINT32_MAX;

  uint64_t prefetch64 = UINT64_MAX;

  while (pos + 4 <= len) {
    if (!prev_had_zero) {
      // since we know that start code prefix has zeros, we find the next dword that has zeros
      while (!cur_has_zero && pos + 8 <= len) {
        prefetch64 = *(uint64_t*)(data + pos);
#if __BYTE_ORDER == __LITTLE_ENDIAN
        cur_has_zero = haszero64_le(prefetch64);
#else
        cur_has_zero = haszero64_be(prefetch64);
#endif
        if (!cur_has_zero) {
          pos += 8;
        }
      }
    }

    if (pos + 4 <= len) {
      cur_value32 = *(uint32_t*)(data + pos);
      // uint8_t* curr = (uint8_t*)&cur_value32;
      // uint8_t* prev = (uint8_t*)&prev_value32;
      // UVG_LOG_DEBUG("P: %u %u %u %u C: %u %u %u %u", prev[0], prev[1], prev[2], prev[3], curr[0],
      // curr[1], curr[2], curr[3]);

#if __BYTE_ORDER == __LITTLE_ENDIAN
      cur_has_zero = haszero32_le(cur_value32);
#else
      cur_has_zero = haszero32_be(cur_value32);
#endif
    }

    if ((prev_had_zero || cur_has_zero) && pos + 4 <= len) {
      /* Previous dword had zeros but this doesn't. The only way there might be a start code
       * is if the most significant byte of current dword is 0x01 */
      if (prev_had_zero) {
        /* previous dword: 0xXX000000 or 0xXXXX0000 and current dword 0x01XXXXXX */
#if __BYTE_ORDER == __LITTLE_ENDIAN
        if (((cur_value32 >> 0) & 0xff) == 0x01 && ((prev_value32 >> 16) & 0xffff) == 0) {
          start_len = (((prev_value32 >> 8) & 0xffffff) == 0) ? 4 : 3;
#else
        if (((cur_value32 >> 24) & 0xff) == 0x01 && ((prev_value32 >> 0) & 0xffff) == 0) {
          start_len = (((prev_value32 >> 0) & 0xffffff) == 0) ? 4 : 3;
#endif
          return pos + 1;
        }
      }

      // find out if the current value as a whole contains start code prefix
      bool additional_byte = false;
      uint8_t ret          = determine_start_prefix_precense(cur_value32, additional_byte);
      start_len            = ret;
      if (ret > 0) {
        if (ret == 5) {
          // ret 5 means we don't know how long the start code is so we check it

          ret = 3;
#if __BYTE_ORDER == __LITTLE_ENDIAN
          start_len = (((prev_value32 >> 24) & 0xff) == 0) ? 4 : 3;
#else
          start_len = (((prev_value32 >> 0) & 0xff) == 0) ? 4 : 3;
#endif
        }
        if (additional_byte) {
          --start_len;
        }

        return pos + ret;
      }

      // see if the start code prefix is split between previous and this dword
#if __BYTE_ORDER == __LITTLE_ENDIAN
      uint16_t cur_ls  = (cur_value32 >> 16) & 0xffff;   // current less significant word
      uint16_t cur_ms  = (cur_value32 >> 0) & 0xffff;    // current more significant word
      uint16_t prev_ls = (prev_value32 >> 16) & 0xffff;  // previous less significant word

      // previous has 4 zeros
      bool p4z = (prev_ls == 0);

      // previous has 2 zeros
      bool p2z = (((prev_ls >> 8) & 0xff) == 0);

      // current has 2 bytes of possible start code
      // bool c2s = (((cur_ms >> 8) & 0xff)              == 0x01);

      // current has 4 bytes of possible start code
      bool c4s = (cur_ms == 0x0100);  // current starts with 0001

      // current has 6 bytes of start code
      bool c6s = (cur_ms == 0x0000 && (cur_ls & 0xff) == 0x01);  // current is 0000 01XX

#else
      uint16_t cur_ls  = (cur_value32 >> 0) & 0xffff;
      uint16_t cur_ms  = (cur_value32 >> 16) & 0xffff;
      uint16_t prev_ls = (prev_value32 >> 0) & 0xffff;

      bool p4z = (prev_ls == 0);
      bool p2z = ((prev_ls & 0xff) == 0);
      // bool c2s = ((cur_ms & 0xff)    == 0x01);
      bool c4s = (cur_ms == 0x0001);
      bool c6s = (cur_ms == 0x0000 && ((cur_ls >> 8) & 0xff) == 0x01);
#endif
      // all possible start code modes between two bytes
      if (p4z && c4s) {
        // previous dword 0xXXXX0000 and current dword is 0x0001XXXX
        start_len = 4;

        return pos + 2;
      } else if (p2z) {
        // Previous dword was 0xXXXXXX00

        if (c6s) {
          // Current dword is 0x000001XX
          start_len = 4;

          return pos + 3;
        } else if (c4s) {
          // Current dword is 0x0001XXXX
          start_len = 3;

          return pos + 2;
        }
      }
    }

    pos += get_start_code_range();

    if (get_start_code_range() == 4) {
      prev_had_zero = cur_has_zero;
      prev_value32  = cur_value32;
    } else {
      prev_value32 =
          (prev_value32 >> 8 * get_start_code_range()) | (cur_value32 << 8 * (4 - get_start_code_range()));

#if __BYTE_ORDER == __LITTLE_ENDIAN
      prev_had_zero = haszero32_le(prev_value32);
#else
      prev_had_zero = haszero32_be(prev_value32);
#endif
    }
  }

  return -1;
}

rtp_error_t uvgrtp::formats::h26x::frame_getter(uvgrtp::frame::rtp_frame** frame) {
  if (queued_.size()) {
    *frame = queued_.front();
    queued_.pop_front();
    return RTP_PKT_READY;
  }

  return RTP_NOT_FOUND;
}

rtp_error_t uvgrtp::formats::h26x::push_media_frame(sockaddr_in& addr, sockaddr_in6& addr6, uint8_t* data,
                                                    size_t data_len, int rtp_flags, uint32_t ssrc) {
  rtp_error_t ret = RTP_OK;

  if (!data || !data_len) return RTP_INVALID_VALUE;

  /* This is the first call of init_transaction. It generates a new RTP timestamp. The init_transaction()
     calls below will use the same RTP timestamp. */
  if ((ret = fqueue_->init_transaction(data)) != RTP_OK) {
    UVG_LOG_ERROR("Invalid frame queue or failed to initialize transaction!");
    return ret;
  }

  size_t payload_size = rtp_ctx_->get_payload_size();

  // find all the locations of NAL units using Start Code Lookup (SCL)
  std::vector<nal_info> nals;
  bool should_aggregate = false;

  rtp_format_t fmt = rtp_ctx_->get_payload();

  if ((rtp_flags & RTP_NO_H26X_SCL) || (fmt == RTP_FORMAT_ATLAS)) {
    nal_info nal;
    nal.offset         = 0;
    nal.prefix_len     = 0;
    nal.size           = data_len;
    nal.aggregate      = false;
    nal.was_aggregated = false;

    nals.push_back(nal);
  } else {
    scl(data, data_len, payload_size, nals, should_aggregate);
  }

  if (nals.empty()) {
    UVG_LOG_ERROR("Did not find any NAL units in frame. Cannot send.");
    return RTP_INVALID_VALUE;
  }

  bool do_not_aggr = (rtp_flags & RTP_H26X_DO_NOT_AGGR);

  if (should_aggregate && !do_not_aggr)  // an aggregate packet is possible
  {
    // use aggregation function that also may just send the packets as Single NAL units
    // if aggregates have not been implemented

    for (auto& nal : nals) {
      if (nal.aggregate) {
        nal.was_aggregated = true;
        if ((ret = add_aggregate_packet(data + nal.offset, nal.size)) != RTP_OK) {
          clear_aggregation_info();
          fqueue_->deinit_transaction();
          return ret;
        }
      } else {
        break;
      }
    }

    (void)finalize_aggregation_pkt();
    // actually send the packets
    ret = fqueue_->flush_queue(addr, addr6, ssrc);
    clear_aggregation_info();
  }

  for (auto& nal : nals)  // non-aggregatable NAL units
  {
    if (do_not_aggr || !nal.was_aggregated || !should_aggregate) {
      if ((ret = fqueue_->init_transaction(data + nal.offset, true)) != RTP_OK) {
        UVG_LOG_ERROR("Invalid frame queue or failed to initialize transaction!");
        return ret;
      }
      // single NAL unit uses the NAL unit header as the payload header meaning that it does not
      // add anything extra to the packet and we can just compare the NAL size with the payload size allowed
      if (nal.size <= payload_size)  // send as a single NAL unit packet
      {
        ret = single_nal_unit(data + nal.offset, nal.size);
      } else  // send divided based on payload_size
      {
        ret = fu_division(&data[nal.offset], nal.size, payload_size);
      }

      if (ret != RTP_OK) {
        clear_aggregation_info();
        fqueue_->deinit_transaction();
        return ret;
      }
      ret = fqueue_->flush_queue(addr, addr6, ssrc);
    }
  }
  return ret;
}

rtp_error_t uvgrtp::formats::h26x::add_aggregate_packet(uint8_t* data, size_t data_len) {
  // the default implementation is to just use single NAL units and don't do the aggregate packet
  return single_nal_unit(data, data_len);
}

rtp_error_t uvgrtp::formats::h26x::finalize_aggregation_pkt() { return RTP_OK; }

void uvgrtp::formats::h26x::clear_aggregation_info() {}

rtp_error_t uvgrtp::formats::h26x::single_nal_unit(uint8_t* data, size_t data_len) {
  // single NAL unit packets use NAL header directly as payload header so the packet is
  // correct as is
  rtp_error_t ret = RTP_OK;
  if ((ret = fqueue_->enqueue_message(data, data_len)) != RTP_OK) {
    UVG_LOG_ERROR("Failed to enqueue single h26x NAL Unit packet!");
  }

  return ret;
}

rtp_error_t uvgrtp::formats::h26x::divide_frame_to_fus(uint8_t* data, size_t& data_left,
                                                       size_t payload_size, uvgrtp::buf_vec& buffers,
                                                       uint8_t fu_headers[]) {
  if (data_left <= payload_size) {
    UVG_LOG_ERROR("Cannot use FU division for packets smaller than payload size");
    return RTP_GENERIC_ERROR;
  }

  rtp_error_t ret = RTP_OK;

  // the FU structure has both payload header and an fu header
  size_t fu_payload_size = payload_size - get_payload_header_size() - get_fu_header_size();

  // skip NAL header of data since it is incorporated in payload and fu headers (which are repeated
  // for each packet, but NAL header is only at the beginning of NAL unit)
  size_t data_pos = get_nal_header_size();
  data_left -= get_nal_header_size();

  while (data_left > fu_payload_size) {
    /* This seems to work by always using the payload headers in first and fu headers in the second index
     * of buffer (and modifying those) and replacing the payload in third, then sending all.
     * The headers for first fragment are already in buffers.at(1) */

    // set the payload for this fragment
    buffers.at(2).first  = fu_payload_size;
    buffers.at(2).second = &data[data_pos];

    if ((ret = fqueue_->enqueue_message(buffers)) != RTP_OK) {
      UVG_LOG_ERROR("Queueing the FU packet failed!");
      return ret;
    }

    data_pos += fu_payload_size;
    data_left -= fu_payload_size;

    buffers.at(1).second = &fu_headers[1];  // middle fragment header
  }

  buffers.at(1).second = &fu_headers[2];  // last fragment header

  // set payload for the last fragment
  buffers.at(2).first  = data_left;
  buffers.at(2).second = &data[data_pos];

  // send the last fragment
  if ((ret = fqueue_->enqueue_message(buffers)) != RTP_OK) {
    UVG_LOG_ERROR("Failed to send the last fragment of an H26x frame!");
  }

  return ret;
}

void uvgrtp::formats::h26x::initialize_fu_headers(uint8_t nal_type, uint8_t fu_headers[]) {
  fu_headers[0] = (uint8_t)((1 << 7) | nal_type);
  fu_headers[1] = nal_type;
  fu_headers[2] = (uint8_t)((1 << 6) | nal_type);
}

uvgrtp::frame::rtp_frame* uvgrtp::formats::h26x::allocate_rtp_frame_with_startcode(
    bool add_start_code, uvgrtp::frame::rtp_header& header, size_t payload_size_without_startcode,
    size_t& fptr) {
  uvgrtp::frame::rtp_frame* complete = uvgrtp::frame::alloc_rtp_frame();

  complete->payload_len = payload_size_without_startcode;

  if (add_start_code) {
    complete->payload_len += 4;
  }

  complete->payload = new uint8_t[complete->payload_len];

  if (add_start_code && complete->payload_len >= 4) {
    complete->payload[0] = 0;
    complete->payload[1] = 0;
    complete->payload[2] = 0;
    complete->payload[3] = 1;
    fptr += 4;
  }

  complete->header = header;  // copy

  return complete;
}

void uvgrtp::formats::h26x::prepend_start_code(int rce_flags, uvgrtp::frame::rtp_frame** out) {
  rtp_format_t fmt = rtp_ctx_->get_payload();
  if (fmt == RTP_FORMAT_ATLAS) {
    return;
  }
  if (!(rce_flags & RCE_NO_H26X_PREPEND_SC)) {
    uint8_t* pl = new uint8_t[(*out)->payload_len + 4];

    pl[0] = 0;
    pl[1] = 0;
    pl[2] = 0;
    pl[3] = 1;

    std::memcpy(pl + 4, (*out)->payload, (*out)->payload_len);
    delete[] (*out)->payload;

    (*out)->payload = pl;
    (*out)->payload_len += 4;
  }
}

size_t uvgrtp::formats::h26x::drop_access_unit(uint32_t ts) {
  size_t total_cleaned = 0;
  if (access_units_.find(ts) == access_units_.end()) {
    UVG_LOG_ERROR("Tried to drop a non-existing frame");
    return total_cleaned;
  }

  /*
  uint16_t s_seq = frames_.at(ts).s_seq;
  uint16_t e_seq = frames_.at(ts).e_seq;

  UVG_LOG_INFO("Dropping frame. Ts: %lu, Seq: %u <-> %u, received/expected: %lli/%lli",
      ts, s_seq, e_seq, frames_[ts].received_packet_seqs.size(), calculate_expected_fus(ts));
  */

  for (auto& fragment_seq : access_units_[ts].incomplete_packet_seqs) {
    total_cleaned += fragments_[fragment_seq]->payload_len + sizeof(uvgrtp::frame::rtp_frame);
    free_fragment(fragment_seq);
  }

  dropped_ts_[ts] = access_units_.at(ts).sframe_time;
  dropped_in_order_.insert(ts);

  if (dropped_ts_.size() > 600) {
    uint32_t oldest_ts = *dropped_in_order_.begin();
    dropped_ts_.erase(oldest_ts);
    dropped_in_order_.erase(oldest_ts);
  }

  access_units_.erase(ts);

  discard_until_key_frame_ = true;

  return total_cleaned;
}

rtp_error_t uvgrtp::formats::h26x::handle_aggregation_packet(uvgrtp::frame::rtp_frame** out,
                                                             uint8_t payload_header_size, int rce_flags) {
  uvgrtp::buf_vec nalus;

  size_t size = 0;
  auto* frame = *out;

  for (size_t i = payload_header_size; i < frame->payload_len;
       i += ntohs(*(uint16_t*)&frame->payload[i]) + sizeof(uint16_t)) {
    uint16_t packet_size = ntohs(*(uint16_t*)&frame->payload[i]);
    size += packet_size;

    if (size <= (*out)->payload_len) {
      nalus.push_back(std::make_pair(packet_size, &frame->payload[i] + sizeof(uint16_t)));
    } else {
      UVG_LOG_ERROR("The received aggregation packet claims to be larger than packet!");
      return RTP_GENERIC_ERROR;
    }
  }

  for (size_t i = 0; i < nalus.size(); ++i) {
    size_t fptr = 0;

    bool prepend_startcode = !(rce_flags & RCE_NO_H26X_PREPEND_SC);
    uvgrtp::frame::rtp_frame* retframe =
        allocate_rtp_frame_with_startcode(prepend_startcode, (*out)->header, nalus[i].first, fptr);

    std::memcpy(retframe->payload + fptr, nalus[i].second, nalus[i].first);

    queued_.push_back(retframe);
  }

  return RTP_MULTIPLE_PKTS_READY;
}

bool uvgrtp::formats::h26x::is_duplicate_frame(uint32_t timestamp, uint16_t seq_num) {
  // if (received_info_.find(timestamp) != received_info_.end()) {
  //     if (std::find(received_info_.at(timestamp).begin(), received_info_.at(timestamp).end(), seq_num) !=
  //     received_info_.at(timestamp).end()) {
  //         UVG_LOG_WARN("duplicate ts and seq num received, discarding frame");
  //         return true;
  //     }
  // }
  pkt_stats stats = {timestamp, seq_num};

  // // Save the received ts and seq num
  // if (received_info_.find(timestamp) == received_info_.end()) {
  //     received_info_.insert({timestamp, {seq_num}});
  // }
  // else {
  //     received_info_.at(timestamp).push_back(seq_num);
  // }
  received_frames_.push_back(stats);

  if (received_frames_.size() > RECEIVED_FRAMES) {
    received_info_.erase(received_frames_.front().ts);
    received_frames_.pop_front();
  }
  return false;
}

rtp_error_t uvgrtp::formats::h26x::packet_handler(void* args, int rce_flags, uint8_t* read_ptr, size_t size,
                                                  uvgrtp::frame::rtp_frame** out) {
  (void)args;
  (void)read_ptr;
  (void)size;
  uvgrtp::frame::rtp_frame* frame = *out;

  if (is_duplicate_frame(frame->header.timestamp, frame->header.seq)) {
    (void)uvgrtp::frame::dealloc_frame(*out);
    *out = nullptr;
    return RTP_GENERIC_ERROR;
  }

  // aggregate, start, middle, end or single NAL
  uvgrtp::formats::FRAG_TYPE frag_type = get_fragment_type(frame);

  // first we check that this packet does not belong to an access unit that has been dropped by garbage
  // collection
  if (dropped_ts_.find(frame->header.timestamp) != dropped_ts_.end()) {
    UVG_LOG_DEBUG("Received an RTP packet belonging to an old, dropped access unit! Timestamp: %u, seq: %u",
                  frame->header.timestamp, frame->header.seq);
    (void)uvgrtp::frame::dealloc_frame(frame);  // free fragment memory
    return RTP_GENERIC_ERROR;
  }

  if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_AGGR) {
    // handle aggregate packets (packets with multiple NAL units in them)
    return handle_aggregation_packet(out, get_payload_header_size(), rce_flags);
  } else if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_STAP_B) {
    // Handle H264 STAP-B packet, RFC 6184 5.7.1
    // DON is made up of the 16 bits after STAP-B header.
    // Commented out to prevent werrors, DON is not currently used anywhere.
    /* uint16_t don = ((uint16_t)frame->payload[1] << 8) | frame->payload[2]; */

    // payload_header_size + 2 comes from DON field in STAP-B packets being 16 bits long
    return handle_aggregation_packet(out, get_payload_header_size() + 2, rce_flags);
  } else if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_NOT_FRAG) {  // Single NAL unit

    // TODO: Check if previous dependencies have been sent forward

    // TODO: We should detect duplicate packets, but there are legitimate situations
    //  where single NAL units have same timestamps
    // completed_ts_[frame->header.timestamp] = std::chrono::high_resolution_clock::now();

    // nothing special needs to be done, just possibly add start codes back
    prepend_start_code(rce_flags, out);
    return RTP_PKT_READY;
  } else if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_INVALID) {
    // something is wrong
    UVG_LOG_WARN("invalid frame received!");
    (void)uvgrtp::frame::dealloc_frame(*out);
    *out = nullptr;
    return RTP_GENERIC_ERROR;
  }

  // We have received a fragment. Rest of the function deals with fragmented frames

  // Fragment timestamp, all fragments of the same frame have the same timestamp
  uint32_t fragment_ts = frame->header.timestamp;

  // Fragment sequence number, determines the order of the fragments within frame
  uint16_t fragment_seq = frame->header.seq;

  uvgrtp::formats::NAL_TYPE nal_type = get_nal_type(frame);  // Intra, inter or some other type of frame

  // UVG_LOG_DEBUG("Received FU, ts: %lu, Seq: %u", fragment_ts, fragment_seq);

  // Initialize new access unit if this is the first packet with this timestamp
  if (access_units_.find(fragment_ts) == access_units_.end()) {
    initialize_new_access_unit(fragment_ts);
    // UVG_LOG_DEBUG("intialized new access unit, ts %u, seq %u", fragment_ts, fragment_seq);
  } else if (access_units_[fragment_ts].received_packet_seqs.find(fragment_seq)
             != access_units_[fragment_ts].received_packet_seqs.end()) {
    // we have already received this seq
    UVG_LOG_DEBUG("Detected duplicate fragment, dropping! Fragment ts: %lu, Seq: %u", fragment_ts,
                  fragment_seq);
    (void)uvgrtp::frame::dealloc_frame(frame);  // free fragment memory
    *out = nullptr;
    return RTP_GENERIC_ERROR;
  }

  const uint8_t sizeof_fu_headers = (uint8_t)get_payload_header_size() + get_fu_header_size();
  access_unit_info& au            = access_units_[fragment_ts];
  // keep track of fragments belonging to this frame
  au.received_packet_seqs.insert(fragment_seq);
  au.incomplete_packet_seqs.insert(fragment_seq);
  au.fragments_info[fragment_seq] = {false, false, false};

  au.total_size += (frame->payload_len - sizeof_fu_headers);

  // This may not be necessary, as new duplicate fragments should get dropped already above
  if (fragments_[fragment_seq] != nullptr) {
    UVG_LOG_WARN(
        "Found an existing fragment with same sequence number %u! Fragment ts: %lu, current ts: %lu",
        fragment_seq, fragments_[fragment_seq]->header.timestamp, fragment_ts);

    free_fragment(fragment_seq);
  }

  // save the fragment for later reconstruction
  fragments_[fragment_seq] = frame;

  // if this is first or last, save it to help with reconstruction
  if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_START) {
    au.fragments_info[fragment_seq].start = true;
  } else if (frag_type == uvgrtp::formats::FRAG_TYPE::FT_END) {
    // au.end_seqs.push_back(fragment_seq);
    au.fragments_info[fragment_seq].end = true;
  }

  /* Used to keep track of fragments that can be used for reconstruction.
  If a NAL unit is reconstructed, these can be removed from incomplete_packet_seqs after the loop below. */
  std::unordered_map<uint16_t, nal> reconstructed_fragments = {};

  // figure out if we have the start sequence number and which seq it is (usually first, but not always)
  rtp_error_t ret    = RTP_OK;
  uint16_t start_seq = 0;
  bool start_found   = false;
  for (const uint16_t& fragment : au.incomplete_packet_seqs) {
    if (au.fragments_info[fragment].start) {
      start_seq                         = fragment;
      start_found                       = true;
      reconstructed_fragments[fragment] = {};  // If this is start FU, initialize a new map for it
      reconstructed_fragments.at(start_seq).seqs.insert(start_seq);
      break;
    }
  }

  if (!start_found) {
    return ret;
  }

  /* Check all the incomplete fragments of this access unit, by looking for a set of fragments with
 consecutive sequence numbers that starts with a start fragment and ends in an end fragment. If this is
 found, a NAL unit can be reconstructed. */
  uint16_t next_seq         = next_seq_num(start_seq);
  uint16_t current_fragment = nextSeq(start_seq, au.incomplete_packet_seqs);

  while (current_fragment != start_seq) {  // go until we reconstruct or find the start again

    bool is_end = au.fragments_info[current_fragment].end;

    // check that this frament is continuous
    if (current_fragment != next_seq) {
      break;
    } else {
      reconstructed_fragments.at(start_seq).seqs.insert(current_fragment);
    }

    /* A continuous set of fragments with a start and end has been found. NAL unit can be reconstructed */
    if (is_end) {
      size_t nal_size = 0;  // Find size of the complete reconstructed NAL unit
      for (auto p : reconstructed_fragments.at(start_seq).seqs) {
        nal_size += (fragments_[p]->payload_len - sizeof_fu_headers);
        au.fragments_info[p].reconstructed = true;
      }

      /* Work in progress feature: here we discard inter frames if their references were not received
       * correctly */
      bool enable_reference_discarding = (rce_flags & RCE_H26X_DEPENDENCY_ENFORCEMENT);
      if (discard_until_key_frame_ && enable_reference_discarding) {
        if (nal_type == uvgrtp::formats::NAL_TYPE::NT_INTER) {
          UVG_LOG_WARN(
              "Dropping h26x access unit because of missing reference. Timestamp: %lu. Seq: %u - %u",
              fragment_ts, *access_units_[fragment_ts].received_packet_seqs.begin(),
              *access_units_[fragment_ts].received_packet_seqs.rbegin());

          drop_access_unit(fragment_ts);
          return RTP_GENERIC_ERROR;
        } else if (nal_type == uvgrtp::formats::NAL_TYPE::NT_INTRA) {
          // we don't have to discard anymore
          UVG_LOG_INFO("Found a key frame at ts %lu", fragment_ts);
          discard_until_key_frame_ = false;
        }
      }
      if ((reconstruction(out, nal_size, rce_flags, start_seq, current_fragment, sizeof_fu_headers))
          == RTP_PKT_READY) {
        ret = RTP_PKT_READY;
      }
      reconstructed_fragments.at(start_seq).complete = true;
    }

    next_seq         = next_seq_num(next_seq);
    current_fragment = nextSeq(current_fragment, au.incomplete_packet_seqs);
  }

  for (auto& p : reconstructed_fragments) {
    if (p.second.complete) {
      for (auto& print : p.second.seqs) {
        // UVG_LOG_DEBUG("reconstructed from fragment %u", print);
        au.incomplete_packet_seqs.erase(print);
      }
    }
  }
  // make sure uvgRTP does not reserve increasing amounts of memory by deleting old access unit information
  garbage_collect_lost_frames(rtp_ctx_->get_pkt_max_delay());
  return ret;
}

void uvgrtp::formats::h26x::garbage_collect_lost_frames(size_t timout) {
  if (uvgrtp::clock::hrc::diff_now(last_garbage_collection_) >= GARBAGE_COLLECTION_INTERVAL_MS) {
    size_t total_cleaned = 0;
    std::vector<uint32_t> to_remove;
    // first find all access units that have been waiting for too long
    for (auto& gc_frame : access_units_) {
      if (uvgrtp::clock::hrc::diff_now(gc_frame.second.sframe_time) > timout) {
#ifndef __RTP_SILENT__
        // uint16_t s_seq = *gc_frame.second.received_packet_seqs.begin();
        // uint16_t e_seq = *gc_frame.second.received_packet_seqs.rbegin();
        // UVG_LOG_DEBUG("Found an old access unit. Ts: %lu, Seq: %u <-> %u, received: %lli",
        // gc_frame.first, s_seq, e_seq, gc_frame.second.received_packet_seqs.size());
#endif
        to_remove.push_back(gc_frame.first);
      }
    }

    // remove old access units
    for (auto& old_frame : to_remove) {
      // UVG_LOG_DEBUG("Dropping old access unit. Ts: %lu", old_frame);
      total_cleaned += drop_access_unit(old_frame);
    }

    if (total_cleaned > 0) {
      UVG_LOG_DEBUG("Garbage collection cleaned %d bytes!", total_cleaned);
    }

    last_garbage_collection_ = uvgrtp::clock::hrc::now();
  }
}

void uvgrtp::formats::h26x::initialize_new_access_unit(uint32_t ts) {
  access_units_[ts].received_packet_seqs = {};
  access_units_[ts].fragments_info       = {};

  access_units_[ts].sframe_time = uvgrtp::clock::hrc::now();
  access_units_[ts].total_size  = 0;
}

uint16_t uvgrtp::formats::h26x::next_seq_num(uint16_t seq) {
  if (seq == UINT16_MAX) {
    return 0;
  } else {
    seq++;
    return seq;
  }
}

void uvgrtp::formats::h26x::free_fragment(uint16_t sequence_number) {
  if (fragments_[sequence_number] == nullptr) {
    UVG_LOG_ERROR("Tried to free an already freed fragment with seq: %u", sequence_number);
    return;
  }

  (void)uvgrtp::frame::dealloc_frame(fragments_[sequence_number]);  // free fragment memory
  fragments_[sequence_number] = nullptr;
  fragments_.erase(sequence_number);
}

void uvgrtp::formats::h26x::scl(uint8_t* data, size_t data_len, size_t packet_size,
                                std::vector<nal_info>& nals, bool& can_be_aggregated) {
  uint8_t start_len = 0;
  ssize_t offset    = find_h26x_start_code(data, data_len, 0, start_len);

  packet_size -= get_payload_header_size();  // aggregate packet has a payload header

  while (offset > -1) {
    nal_info nal;
    nal.offset     = size_t(offset);
    nal.prefix_len = start_len;
    nal.size       = 0;      // set after all NALs have been found
    nal.aggregate  = false;  // determined with size calculations

    nals.push_back(nal);
    offset = find_h26x_start_code(data, data_len, offset, start_len);
  }

  size_t aggregate_size    = 0;
  int aggregatable_packets = 0;

  // calculate the sizes of NAL units
  for (size_t i = 0; i < nals.size(); ++i) {
    if (nals.size() > i + 1) {
      // take the difference of next NAL unit location and current one,
      // minus size of start code prefix of next NAL unit
      nals.at(i).size = nals[i + 1].offset - nals[i].offset - nals[i + 1].prefix_len;
    } else {
      // last NAL unit, the length is offset to end
      nals.at(i).size = data_len - nals[i].offset;
    }

    // each NAL unit added to aggregate packet needs the size added which has to be taken into account
    // when calculating the aggregate packet
    // (NOTE: This is not enough for MTAP in h264, but I doubt uvgRTP will support it)
    if (aggregate_size + nals.at(i).size + sizeof(uint16_t) <= packet_size) {
      aggregate_size += nals.at(i).size + sizeof(uint16_t);
      nals.at(i).aggregate = true;
      ++aggregatable_packets;
    }
  }

  can_be_aggregated = (aggregatable_packets >= 2);
}

rtp_error_t uvgrtp::formats::h26x::reconstruction(uvgrtp::frame::rtp_frame** out, size_t nal_size,
                                                  int rce_flags, uint16_t s_seq, uint16_t e_seq,
                                                  const uint8_t sizeof_fu_headers) {
  uvgrtp::frame::rtp_frame* frame = *out;
  // UVG_LOG_DEBUG("Reconstructing frame. Ts: %lu, Seq: %u -> %u", ts, s_seq, e_seq);

  // Reconstruction of frame from fragments
  size_t fptr = 0;

  // allocating the frame with start code ready saves a copy operation for the frame
  bool start_code = !(rce_flags & RCE_NO_H26X_PREPEND_SC);
  if (rtp_ctx_->get_payload() == RTP_FORMAT_ATLAS) {
    start_code = false;
  }
  uvgrtp::frame::rtp_frame* complete =
      allocate_rtp_frame_with_startcode(start_code, frame->header, get_nal_header_size() + nal_size, fptr);

  // construct the NAL header from fragment header of current fragment
  get_nal_header_from_fu_headers(fptr, frame->payload, complete->payload);  // NAL header
  fptr += get_nal_header_size();

  // uint16_t next_from_last = frames_.at(frame_timestamp).e_seq + 1;
  uint16_t next_from_last = uint16_t(next_seq_num(e_seq));
  for (uint16_t i = s_seq; i != next_from_last; ++i) {
    if (fragments_[i] == nullptr) {
      UVG_LOG_ERROR("Missing fragment in reconstruction. Seq range: %u - %u. Missing seq %u", s_seq, e_seq,
                    i);
      return RTP_GENERIC_ERROR;
    }

    // copy everything expect fu headers (which repeat for every fu)
    std::memcpy(&complete->payload[fptr], &fragments_[i]->payload[sizeof_fu_headers],
                fragments_[i]->payload_len - sizeof_fu_headers);
    fptr += fragments_[i]->payload_len - sizeof_fu_headers;
    free_fragment(i);
  }

  *out = complete;       // save result to output
  return RTP_PKT_READY;  // indicate that we have a frame ready
}

uint16_t uvgrtp::formats::h26x::nextSeq(uint16_t currentSeq, std::set<uint16_t>& fragments) {
  auto currentIterator = fragments.find(currentSeq);

  if (*currentIterator == *(fragments.rbegin())) {
    return *(fragments.begin());
  }

  return *(++currentIterator);
}