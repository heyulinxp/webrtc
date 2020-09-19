/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/timing.h"

#include <assert.h>

#include <algorithm>

#include "rtc_base/time/timestamp_extrapolator.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

VCMTiming::VCMTiming(Clock* clock, VCMTiming* master_timing)
    : clock_(clock),
      master_(false),
      ts_extrapolator_(),
      codec_timer_(new VCMCodecTimer()),
      render_delay_ms_(kDefaultRenderDelayMs),
      min_playout_delay_ms_(0),
      max_playout_delay_ms_(10000),
      jitter_delay_ms_(0),
      current_delay_ms_(0),
      prev_frame_timestamp_(0),
      timing_frame_info_(),
      num_decoded_frames_(0) {
  if (master_timing == NULL) {
    master_ = true;
    ts_extrapolator_ = new TimestampExtrapolator(clock_->TimeInMilliseconds());
  } else {
    ts_extrapolator_ = master_timing->ts_extrapolator_;
  }
}

VCMTiming::~VCMTiming() {
  if (master_) {
    delete ts_extrapolator_;
  }
}

void VCMTiming::Reset() {
  MutexLock lock(&mutex_);
  ts_extrapolator_->Reset(clock_->TimeInMilliseconds());
  codec_timer_.reset(new VCMCodecTimer());
  render_delay_ms_ = kDefaultRenderDelayMs;
  min_playout_delay_ms_ = 0;
  jitter_delay_ms_ = 0;
  current_delay_ms_ = 0;
  prev_frame_timestamp_ = 0;
}

void VCMTiming::set_render_delay(int render_delay_ms) {
  MutexLock lock(&mutex_);
  render_delay_ms_ = render_delay_ms;
}

void VCMTiming::set_min_playout_delay(int min_playout_delay_ms) {
  MutexLock lock(&mutex_);
  min_playout_delay_ms_ = min_playout_delay_ms;
}

int VCMTiming::min_playout_delay() {
  MutexLock lock(&mutex_);
  return min_playout_delay_ms_;
}

void VCMTiming::set_max_playout_delay(int max_playout_delay_ms) {
  MutexLock lock(&mutex_);
  max_playout_delay_ms_ = max_playout_delay_ms;
}

int VCMTiming::max_playout_delay() {
  MutexLock lock(&mutex_);
  return max_playout_delay_ms_;
}

//设置视频必须在接收器上延迟的最短时间，以获得所需的抖动缓冲级别
void VCMTiming::SetJitterDelay(int jitter_delay_ms) {
  MutexLock lock(&mutex_);
  if (jitter_delay_ms != jitter_delay_ms_) {
    jitter_delay_ms_ = jitter_delay_ms;
    // When in initial state, set current delay to minimum delay.
    if (current_delay_ms_ == 0) {
      current_delay_ms_ = jitter_delay_ms_;
    }
  }
}

//更新当前的current_delay_ms_
void VCMTiming::UpdateCurrentDelay(uint32_t frame_timestamp) {
  MutexLock lock(&mutex_);
  int target_delay_ms = TargetDelayInternal();

  if (current_delay_ms_ == 0) {
    // Not initialized, set current delay to target.
    //初始化
    current_delay_ms_ = target_delay_ms;
  } else if (target_delay_ms != current_delay_ms_) {
    int64_t delay_diff_ms =
        static_cast<int64_t>(target_delay_ms) - current_delay_ms_;
    // Never change the delay with more than 100 ms every second. If we're
    // changing the delay in too large steps we will get noticeable freezes. By
    // limiting the change we can increase the delay in smaller steps, which
    // will be experienced as the video is played in slow motion. When lowering
    // the delay the video will be played at a faster pace.
    //千万不要改变每秒超过100毫秒的延迟。如果我们改变延迟的步数太大，我们将得到明显的冻结。
    //通过限制更改，我们可以在较小的步骤中增加延迟，这将在视频以慢动作播放时体验到。
    //当降低延迟时，视频将以更快的速度播放。
    int64_t max_change_ms = 0;
    if (frame_timestamp < 0x0000ffff && prev_frame_timestamp_ > 0xffff0000) {
      // wrap
      max_change_ms = kDelayMaxChangeMsPerS *
                      (frame_timestamp + (static_cast<int64_t>(1) << 32) -
                       prev_frame_timestamp_) /
                      90000;
    } else {
      max_change_ms = kDelayMaxChangeMsPerS *
                      (frame_timestamp - prev_frame_timestamp_) / 90000;
    }

    if (max_change_ms <= 0) {
      // Any changes less than 1 ms are truncated and will be postponed.
      // Negative change will be due to reordering and should be ignored.
      //任何小于1毫秒的更改都将被截断并将被推迟。负变化将是由于重新排序，应忽略。
      return;
    }
    delay_diff_ms = std::max(delay_diff_ms, -max_change_ms);
    delay_diff_ms = std::min(delay_diff_ms, max_change_ms);

    current_delay_ms_ = current_delay_ms_ + delay_diff_ms;
  }
  prev_frame_timestamp_ = frame_timestamp;
}

//更新当前的current_delay_ms_
void VCMTiming::UpdateCurrentDelay(int64_t render_time_ms,
                                   int64_t actual_decode_time_ms) {
  MutexLock lock(&mutex_);
  uint32_t target_delay_ms = TargetDelayInternal();
  int64_t delayed_ms =
      actual_decode_time_ms -
      (render_time_ms - RequiredDecodeTimeMs() - render_delay_ms_);
  if (delayed_ms < 0) {
    return;
  }
  if (current_delay_ms_ + delayed_ms <= target_delay_ms) {
    current_delay_ms_ += delayed_ms;
  } else {
    current_delay_ms_ = target_delay_ms;
  }
}

void VCMTiming::StopDecodeTimer(uint32_t /*time_stamp*/,
                                int32_t decode_time_ms,
                                int64_t now_ms,
                                int64_t /*render_time_ms*/) {
  StopDecodeTimer(decode_time_ms, now_ms);
}

void VCMTiming::StopDecodeTimer(int32_t decode_time_ms, int64_t now_ms) {
  MutexLock lock(&mutex_);
  codec_timer_->AddTiming(decode_time_ms, now_ms);
  assert(decode_time_ms >= 0);
  ++num_decoded_frames_;
}

//新来了一个timestamp
void VCMTiming::IncomingTimestamp(uint32_t time_stamp, int64_t now_ms) {
  MutexLock lock(&mutex_);
  ts_extrapolator_->Update(now_ms, time_stamp);
}

//计算渲染需要的时间
int64_t VCMTiming::RenderTimeMs(uint32_t frame_timestamp,
                                int64_t now_ms) const {
  MutexLock lock(&mutex_);
  return RenderTimeMsInternal(frame_timestamp, now_ms);
}

//计算渲染需要的时间
int64_t VCMTiming::RenderTimeMsInternal(uint32_t frame_timestamp,
                                        int64_t now_ms) const {
  if (min_playout_delay_ms_ == 0 && max_playout_delay_ms_ == 0) {
    // Render as soon as possible.
    return 0;
  }
  //时间外推
  int64_t estimated_complete_time_ms =
      ts_extrapolator_->ExtrapolateLocalTime(frame_timestamp);
  //外推时间计算出错了吧
  if (estimated_complete_time_ms == -1) {
    estimated_complete_time_ms = now_ms;
  }

  // Make sure the actual delay stays in the range of |min_playout_delay_ms_|
  // and |max_playout_delay_ms_|.
  //确保真实的delay时间在范围之间
  int actual_delay = std::max(current_delay_ms_, min_playout_delay_ms_);
  actual_delay = std::min(actual_delay, max_playout_delay_ms_);
  return estimated_complete_time_ms + actual_delay;
}

int VCMTiming::RequiredDecodeTimeMs() const {
  const int decode_time_ms = codec_timer_->RequiredDecodeTimeMs();
  assert(decode_time_ms >= 0);
  return decode_time_ms;
}

//估计的最大等待时间=渲染后的时间-当前时间-编码所需时间-渲染所需时间
int64_t VCMTiming::MaxWaitingTime(int64_t render_time_ms,
                                  int64_t now_ms) const {
  MutexLock lock(&mutex_);

  const int64_t max_wait_time_ms =
      render_time_ms - now_ms - RequiredDecodeTimeMs() - render_delay_ms_;

  return max_wait_time_ms;
}

// Returns the current target delay which is required delay + decode time +
// render delay.
//jitter延时+解码延时+渲染延时
int VCMTiming::TargetVideoDelay() const {
  MutexLock lock(&mutex_);
  return TargetDelayInternal();
}

//计算目标延时，抖动延时+编码延时+渲染后所需时间
int VCMTiming::TargetDelayInternal() const {
  return std::max(min_playout_delay_ms_,
                  jitter_delay_ms_ + RequiredDecodeTimeMs() + render_delay_ms_);
}

bool VCMTiming::GetTimings(int* max_decode_ms,
                           int* current_delay_ms,
                           int* target_delay_ms,
                           int* jitter_buffer_ms,
                           int* min_playout_delay_ms,
                           int* render_delay_ms) const {
  MutexLock lock(&mutex_);
  *max_decode_ms = RequiredDecodeTimeMs();
  *current_delay_ms = current_delay_ms_;
  *target_delay_ms = TargetDelayInternal();
  *jitter_buffer_ms = jitter_delay_ms_;
  *min_playout_delay_ms = min_playout_delay_ms_;
  *render_delay_ms = render_delay_ms_;
  return (num_decoded_frames_ > 0);
}

void VCMTiming::SetTimingFrameInfo(const TimingFrameInfo& info) {
  MutexLock lock(&mutex_);
  timing_frame_info_.emplace(info);
}

absl::optional<TimingFrameInfo> VCMTiming::GetTimingFrameInfo() {
  MutexLock lock(&mutex_);
  return timing_frame_info_;
}

}  // namespace webrtc
