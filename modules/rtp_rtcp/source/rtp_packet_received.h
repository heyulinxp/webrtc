/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef MODULES_RTP_RTCP_SOURCE_RTP_PACKET_RECEIVED_H_
#define MODULES_RTP_RTCP_SOURCE_RTP_PACKET_RECEIVED_H_

#include <stdint.h>

#include <vector>

#include "api/array_view.h"
#include "api/rtp_headers.h"
#include "modules/rtp_rtcp/source/rtp_packet.h"

namespace webrtc {
// Class to hold rtp packet with metadata for receiver side.
class RtpPacketReceived : public RtpPacket {
 public:
  RtpPacketReceived();
  explicit RtpPacketReceived(const ExtensionManager* extensions);
  RtpPacketReceived(const RtpPacketReceived& packet);
  RtpPacketReceived(RtpPacketReceived&& packet);

  RtpPacketReceived& operator=(const RtpPacketReceived& packet);
  RtpPacketReceived& operator=(RtpPacketReceived&& packet);

  ~RtpPacketReceived();

  // TODO(danilchap): Remove this function when all code update to use RtpPacket
  // directly. Function is there just for easier backward compatibilty.
  void GetHeader(RTPHeader* header) const;

  // Time in local time base as close as it can to packet arrived on the
  // network.
  int64_t arrival_time_ms() const { return arrival_time_ms_; }
  void set_arrival_time_ms(int64_t time) { arrival_time_ms_ = time; }

  // Flag if packet was recovered via RTX or FEC.
  //如果通过RTX或FEC恢复数据包，则标记。
  bool recovered() const { return recovered_; }
  void set_recovered(bool value) { recovered_ = value; }

  int payload_type_frequency() const { return payload_type_frequency_; }
  void set_payload_type_frequency(int value) {
    payload_type_frequency_ = value;
  }

  // Additional data bound to the RTP packet for use in application code,
  // outside of WebRTC.
  rtc::ArrayView<const uint8_t> application_data() const {
    return application_data_;
  }
  void set_application_data(rtc::ArrayView<const uint8_t> data) {
    application_data_.assign(data.begin(), data.end());
  }

 private:
  int64_t arrival_time_ms_ = 0;
  int payload_type_frequency_ = 0;
  bool recovered_ = false;
  std::vector<uint8_t> application_data_;
};

}  // namespace webrtc
#endif  // MODULES_RTP_RTCP_SOURCE_RTP_PACKET_RECEIVED_H_
