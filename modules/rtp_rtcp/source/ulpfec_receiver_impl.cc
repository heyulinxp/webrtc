/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/ulpfec_receiver_impl.h"

#include <memory>
#include <utility>

#include "api/scoped_refptr.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

std::unique_ptr<UlpfecReceiver> UlpfecReceiver::Create(
    uint32_t ssrc,
    RecoveredPacketReceiver* callback,
    rtc::ArrayView<const RtpExtension> extensions) {
  return std::make_unique<UlpfecReceiverImpl>(ssrc, callback, extensions);
}

UlpfecReceiverImpl::UlpfecReceiverImpl(
    uint32_t ssrc,
    RecoveredPacketReceiver* callback,
    rtc::ArrayView<const RtpExtension> extensions)
    : ssrc_(ssrc),
      extensions_(extensions),
      recovered_packet_callback_(callback),
      fec_(ForwardErrorCorrection::CreateUlpfec(ssrc_)) {}

UlpfecReceiverImpl::~UlpfecReceiverImpl() {
  received_packets_.clear();
  fec_->ResetState(&recovered_packets_);
}

FecPacketCounter UlpfecReceiverImpl::GetPacketCounter() const {
  MutexLock lock(&mutex_);
  return packet_counter_;
}

//     0                   1                    2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3  4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |F|   block PT  |  timestamp offset         |   block length    |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//
// RFC 2198          RTP Payload for Redundant Audio Data    September 1997
//
//    The bits in the header are specified as follows:
//
//    F: 1 bit First bit in header indicates whether another header block
//        follows.  If 1 further header blocks follow, if 0 this is the
//        last header block.
//        If 0 there is only 1 byte RED header
//
//    block PT: 7 bits RTP payload type for this block.
//
//    timestamp offset:  14 bits Unsigned offset of timestamp of this block
//        relative to timestamp given in RTP header.  The use of an unsigned
//        offset implies that redundant data must be sent after the primary
//        data, and is hence a time to be subtracted from the current
//        timestamp to determine the timestamp of the data for which this
//        block is the redundancy.
//
//    block length:  10 bits Length in bytes of the corresponding data
//        block excluding header.

bool UlpfecReceiverImpl::AddReceivedRedPacket(
    const RtpPacketReceived& rtp_packet,
    uint8_t ulpfec_payload_type) {
  if (rtp_packet.Ssrc() != ssrc_) {
    RTC_LOG(LS_WARNING)
        << "Received RED packet with different SSRC than expected; dropping.";
    return false;
  }
  if (rtp_packet.size() > IP_PACKET_SIZE) {
    RTC_LOG(LS_WARNING) << "Received RED packet with length exceeds maximum IP "
                           "packet size; dropping.";
    return false;
  }
  MutexLock lock(&mutex_);

  static constexpr uint8_t kRedHeaderLength = 1;

  if (rtp_packet.payload_size() == 0) {
    RTC_LOG(LS_WARNING) << "Corrupt/truncated FEC packet.";
    return false;
  }

  // Remove RED header of incoming packet and store as a virtual RTP packet.
  auto received_packet =
      std::make_unique<ForwardErrorCorrection::ReceivedPacket>();
  received_packet->pkt = new ForwardErrorCorrection::Packet();

  // Get payload type from RED header and sequence number from RTP header.
  uint8_t payload_type = rtp_packet.payload()[0] & 0x7f;
  received_packet->is_fec = payload_type == ulpfec_payload_type;
  received_packet->is_recovered = rtp_packet.recovered();
  received_packet->ssrc = rtp_packet.Ssrc();
  received_packet->seq_num = rtp_packet.SequenceNumber();

  if (rtp_packet.payload()[0] & 0x80) {
    // f bit set in RED header, i.e. there are more than one RED header blocks.
    // WebRTC never generates multiple blocks in a RED packet for FEC.
    //WebRTC从不在FEC的红包中生成多个块。
    RTC_LOG(LS_WARNING) << "More than 1 block in RED packet is not supported.";
    return false;
  }

  ++packet_counter_.num_packets;
  packet_counter_.num_bytes += rtp_packet.size();
  if (packet_counter_.first_packet_time_ms == -1) {
    packet_counter_.first_packet_time_ms = rtc::TimeMillis();
  }

  if (received_packet->is_fec) {
    ++packet_counter_.num_fec_packets;
    // everything behind the RED header
    received_packet->pkt->data =
        rtp_packet.Buffer().Slice(rtp_packet.headers_size() + kRedHeaderLength,
                                  rtp_packet.payload_size() - kRedHeaderLength);
  } else {
    auto red_payload = rtp_packet.payload().subview(kRedHeaderLength);
    received_packet->pkt->data.EnsureCapacity(rtp_packet.headers_size() +
                                              red_payload.size());
    // Copy RTP header.
    received_packet->pkt->data.SetData(rtp_packet.data(),
                                       rtp_packet.headers_size());
    // Set payload type.
    uint8_t& payload_type_byte = received_packet->pkt->data.MutableData()[1];
    payload_type_byte &= 0x80;          // Reset RED payload type.
    payload_type_byte += payload_type;  // Set media payload type.
    // Copy payload data.
    received_packet->pkt->data.AppendData(red_payload.data(),
                                          red_payload.size());
  }

  if (received_packet->pkt->data.size() > 0) {
    received_packets_.push_back(std::move(received_packet));
  }
  return true;
}

// TODO(nisse): Drop always-zero return value.
int32_t UlpfecReceiverImpl::ProcessReceivedFec() {
  mutex_.Lock();

  // If we iterate over |received_packets_| and it contains a packet that cause
  // us to recurse back to this function (for example a RED packet encapsulating
  // a RED packet), then we will recurse forever. To avoid this we swap
  // |received_packets_| with an empty vector so that the next recursive call
  // wont iterate over the same packet again. This also solves the problem of
  // not modifying the vector we are currently iterating over (packets are added
  // in AddReceivedRedPacket).
  //如果我们迭代|received_packets_|，它包含一个使我们递归到这个函数的包
  //（例如，一个红包封装了一个红包），
  //那么我们将永远递归。为了避免这种情况，我们用一个空向量交换接收到的数据包，
  //这样下一个递归调用就不会再次迭代同一个数据包。
  //这也解决了不修改当前正在迭代的向量的问题（数据包被添加到addReceivedRepacket中）。
  std::vector<std::unique_ptr<ForwardErrorCorrection::ReceivedPacket>>
      received_packets;
  received_packets.swap(received_packets_);

  for (const auto& received_packet : received_packets) {
    // Send received media packet to VCM.
    //将接收到的媒体包发送到VCM。
    if (!received_packet->is_fec) {
      ForwardErrorCorrection::Packet* packet = received_packet->pkt;
      mutex_.Unlock();
      recovered_packet_callback_->OnRecoveredPacket(packet->data.data(),
                                                    packet->data.size());
      mutex_.Lock();
      // Create a packet with the buffer to modify it.
      RtpPacketReceived rtp_packet;
      const uint8_t* const original_data = packet->data.cdata();
      if (!rtp_packet.Parse(packet->data)) {
        RTC_LOG(LS_WARNING) << "Corrupted media packet";
      } else {
        rtp_packet.IdentifyExtensions(extensions_);
        // Reset buffer reference, so zeroing would work on a buffer with a
        // single reference.
        packet->data = rtc::CopyOnWriteBuffer(0);
        rtp_packet.ZeroMutableExtensions();
        packet->data = rtp_packet.Buffer();
        // Ensure that zeroing of extensions was done in place.
        RTC_DCHECK_EQ(packet->data.cdata(), original_data);
      }
    }
    if (!received_packet->is_recovered) {
      // Do not pass recovered packets to FEC. Recovered packet might have
      // different set of the RTP header extensions and thus different byte
      // representation than the original packet, That will corrupt
      // FEC calculation.
      //不要将恢复的数据包传递给FEC。恢复后的数据包可能有不同的RTP头扩展集，
      //因而和原始数据包不同的字节表示，这将破坏FEC计算。
      fec_->DecodeFec(*received_packet, &recovered_packets_);
    }
  }

  // Send any recovered media packets to VCM.
  //将任何恢复的媒体数据包发送到VCM。
  for (const auto& recovered_packet : recovered_packets_) {
    if (recovered_packet->returned) {
      // Already sent to the VCM and the jitter buffer.
      //抖动已经发送到VCM。
      continue;
    }
    ForwardErrorCorrection::Packet* packet = recovered_packet->pkt;
    ++packet_counter_.num_recovered_packets;
    // Set this flag first; in case the recovered packet carries a RED
    // header, OnRecoveredPacket will recurse back here.
    //首先设置此标志；如果恢复的数据包带有红色标头，OnRecoveredPacket将在此处递归。
    recovered_packet->returned = true;
    mutex_.Unlock();
    recovered_packet_callback_->OnRecoveredPacket(packet->data.data(),
                                                  packet->data.size());
    mutex_.Lock();
  }

  mutex_.Unlock();
  return 0;
}

}  // namespace webrtc
