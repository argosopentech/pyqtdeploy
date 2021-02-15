// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_VIDEO_VIDEO_ENCODE_ACCELERATOR_H_
#define MEDIA_VIDEO_VIDEO_ENCODE_ACCELERATOR_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "base/callback_forward.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/single_thread_task_runner.h"
#include "media/base/bitstream_buffer.h"
#include "media/base/media_export.h"
#include "media/base/video_bitrate_allocation.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_frame.h"
#include "media/video/h264_parser.h"

namespace media {

class BitstreamBuffer;
class VideoFrame;

//  Metadata for a VP8 bitstream buffer.
//  |non_reference| is true iff this frame does not update any reference buffer,
//                  meaning dropping this frame still results in a decodable
//                  stream.
//  |temporal_idx|  indicates the temporal index for this frame.
//  |layer_sync|    if true iff this frame has |temporal_idx| > 0 and does NOT
//                  reference any reference buffer containing a frame with
//                  temporal_idx > 0.
struct MEDIA_EXPORT Vp8Metadata final {
  Vp8Metadata();
  Vp8Metadata(const Vp8Metadata& other);
  Vp8Metadata(Vp8Metadata&& other);
  ~Vp8Metadata();
  bool non_reference;
  uint8_t temporal_idx;
  bool layer_sync;
};

//  Metadata associated with a bitstream buffer.
//  |payload_size| is the byte size of the used portion of the buffer.
//  |key_frame| is true if this delivered frame is a keyframe.
//  |timestamp| is the same timestamp as in VideoFrame passed to Encode().
//  |vp8|, if set, contains metadata specific to VP8. See above.
struct MEDIA_EXPORT BitstreamBufferMetadata final {
  BitstreamBufferMetadata();
  BitstreamBufferMetadata(BitstreamBufferMetadata&& other);
  BitstreamBufferMetadata(size_t payload_size_bytes,
                          bool key_frame,
                          base::TimeDelta timestamp);
  ~BitstreamBufferMetadata();

  size_t payload_size_bytes;
  bool key_frame;
  base::TimeDelta timestamp;
  base::Optional<Vp8Metadata> vp8;
};

// Video encoder interface.
class MEDIA_EXPORT VideoEncodeAccelerator {
 public:
  // Specification of an encoding profile supported by an encoder.
  struct MEDIA_EXPORT SupportedProfile {
    SupportedProfile();
    SupportedProfile(VideoCodecProfile profile,
                     const gfx::Size& max_resolution,
                     uint32_t max_framerate_numerator = 0u,
                     uint32_t max_framerate_denominator = 1u);
    ~SupportedProfile();
    VideoCodecProfile profile;
    gfx::Size min_resolution;
    gfx::Size max_resolution;
    uint32_t max_framerate_numerator;
    uint32_t max_framerate_denominator;
  };
  using SupportedProfiles = std::vector<SupportedProfile>;
  using FlushCallback = base::OnceCallback<void(bool)>;

  // Enumeration of potential errors generated by the API.
  enum Error {
    // An operation was attempted during an incompatible encoder state.
    kIllegalStateError,
    // Invalid argument was passed to an API method.
    kInvalidArgumentError,
    // A failure occurred at the GPU process or one of its dependencies.
    // Examples of such failures include GPU hardware failures, GPU driver
    // failures, GPU library failures, GPU process programming errors, and so
    // on.
    kPlatformFailureError,
    kErrorMax = kPlatformFailureError
  };

  // A default framerate for all VEA implementations.
  enum { kDefaultFramerate = 30 };

  // Parameters required for VEA initialization.
  struct MEDIA_EXPORT Config {
    // Indicates if video content should be treated as a "normal" camera feed
    // or as generated (e.g. screen capture).
    enum class ContentType { kCamera, kDisplay };
    // Indicates the storage type of a video frame provided on Encode().
    // kShmem if a video frame is mapped in user space.
    // kDmabuf if a video frame is referred by dmabuf.
    enum class StorageType { kShmem, kDmabuf };

    Config();
    Config(const Config& config);

    Config(VideoPixelFormat input_format,
           const gfx::Size& input_visible_size,
           VideoCodecProfile output_profile,
           uint32_t initial_bitrate,
           base::Optional<uint32_t> initial_framerate = base::nullopt,
           base::Optional<uint32_t> gop_length = base::nullopt,
           base::Optional<uint8_t> h264_output_level = base::nullopt,
           base::Optional<StorageType> storage_type = base::nullopt,
           ContentType content_type = ContentType::kCamera);

    ~Config();

    std::string AsHumanReadableString() const;

    // Frame format of input stream (as would be reported by
    // VideoFrame::format() for frames passed to Encode()).
    VideoPixelFormat input_format;

    // Resolution of input stream (as would be reported by
    // VideoFrame::visible_rect().size() for frames passed to Encode()).
    gfx::Size input_visible_size;

    // Codec profile of encoded output stream.
    VideoCodecProfile output_profile;

    // Initial bitrate of encoded output stream in bits per second.
    uint32_t initial_bitrate;

    // Initial encoding framerate in frames per second. This is optional and
    // VideoEncodeAccelerator should use |kDefaultFramerate| if not given.
    base::Optional<uint32_t> initial_framerate;

    // Group of picture length for encoded output stream, indicates the
    // distance between two key frames, i.e. IPPPIPPP would be represent as 4.
    base::Optional<uint32_t> gop_length;

    // Codec level of encoded output stream for H264 only. This value should
    // be aligned to the H264 standard definition of SPS.level_idc.
    // If this is not given, VideoEncodeAccelerator selects one of proper H.264
    // levels for |input_visible_size| and |initial_framerate|.
    base::Optional<uint8_t> h264_output_level;

    // The storage type of video frame provided on Encode().
    // If no value is set, VEA doesn't check the storage type of video frame on
    // Encode().
    // This is kShmem iff a video frame is mapped in user space.
    // This is kDmabuf iff a video frame has dmabuf.
    base::Optional<StorageType> storage_type;

    // Indicates captured video (from a camera) or generated (screen grabber).
    // Screen content has a number of special properties such as lack of noise,
    // burstiness of motion and requirements for readability of small text in
    // bright colors. With this content hint the encoder may choose to optimize
    // for the given use case.
    ContentType content_type;
  };

  // Interface for clients that use VideoEncodeAccelerator. These callbacks will
  // not be made unless Initialize() has returned successfully.
  class MEDIA_EXPORT Client {
   public:
    // Callback to tell the client what size of frames and buffers to provide
    // for input and output.  The VEA disclaims use or ownership of all
    // previously provided buffers once this callback is made.
    // Parameters:
    //  |input_count| is the number of input VideoFrames required for encoding.
    //  The client should be prepared to feed at least this many frames into the
    //  encoder before being returned any input frames, since the encoder may
    //  need to hold onto some subset of inputs as reference pictures.
    //  |input_coded_size| is the logical size of the input frames (as reported
    //  by VideoFrame::coded_size()) to encode, in pixels.  The encoder may have
    //  hardware alignment requirements that make this different from
    //  |input_visible_size|, as requested in Initialize(), in which case the
    //  input VideoFrame to Encode() should be padded appropriately.
    //  |output_buffer_size| is the required size of output buffers for this
    //  encoder in bytes.
    virtual void RequireBitstreamBuffers(unsigned int input_count,
                                         const gfx::Size& input_coded_size,
                                         size_t output_buffer_size) = 0;

    // Callback to deliver encoded bitstream buffers.  Ownership of the buffer
    // is transferred back to the VEA::Client once this callback is made.
    // Parameters:
    //  |bitstream_buffer_id| is the id of the buffer that is ready.
    //  |metadata| contains data such as payload size and timestamp. See above.
    virtual void BitstreamBufferReady(
        int32_t bitstream_buffer_id,
        const BitstreamBufferMetadata& metadata) = 0;

    // Error notification callback. Note that errors in Initialize() will not be
    // reported here, but will instead be indicated by a false return value
    // there.
    virtual void NotifyError(Error error) = 0;

   protected:
    // Clients are not owned by VEA instances and should not be deleted through
    // these pointers.
    virtual ~Client() {}
  };

  // Video encoder functions.

  // Returns a list of the supported codec profiles of the video encoder. This
  // can be called before Initialize().
  virtual SupportedProfiles GetSupportedProfiles() = 0;

  // Initializes the video encoder with specific configuration.  Called once per
  // encoder construction.  This call is synchronous and returns true iff
  // initialization is successful.
  // TODO(mcasas): Update to asynchronous, https://crbug.com/744210.
  // Parameters:
  //  |config| contains the initialization parameters.
  //  |client| is the client of this video encoder.  The provided pointer must
  //  be valid until Destroy() is called.
  // TODO(sheu): handle resolution changes.  http://crbug.com/249944
  virtual bool Initialize(const Config& config, Client* client) = 0;

  // Encodes the given frame.
  // The storage type of |frame| must be the |storage_type| if it is specified
  // in Initialize().
  // TODO(crbug.com/895230): Raise an error if the storage types are mismatch.
  // Parameters:
  //  |frame| is the VideoFrame that is to be encoded.
  //  |force_keyframe| forces the encoding of a keyframe for this frame.
  virtual void Encode(scoped_refptr<VideoFrame> frame, bool force_keyframe) = 0;

  // Send a bitstream buffer to the encoder to be used for storing future
  // encoded output.  Each call here with a given |buffer| will cause the buffer
  // to be filled once, then returned with BitstreamBufferReady().
  // Parameters:
  //  |buffer| is the bitstream buffer to use for output.
  virtual void UseOutputBitstreamBuffer(BitstreamBuffer buffer) = 0;

  // Request a change to the encoding parameters. This is only a request,
  // fulfilled on a best-effort basis.
  // Parameters:
  //  |bitrate| is the requested new bitrate, in bits per second.
  //  |framerate| is the requested new framerate, in frames per second.
  virtual void RequestEncodingParametersChange(uint32_t bitrate,
                                               uint32_t framerate) = 0;

  // Request a change to the encoding parameters. This is only a request,
  // fulfilled on a best-effort basis. If not implemented, default behavior is
  // to get the sum over layers and pass to version with bitrate as uint32_t.
  // Parameters:
  //  |bitrate| is the requested new bitrate, per spatial and temporal layer.
  //  |framerate| is the requested new framerate, in frames per second.
  virtual void RequestEncodingParametersChange(
      const VideoBitrateAllocation& bitrate,
      uint32_t framerate);

  // Destroys the encoder: all pending inputs and outputs are dropped
  // immediately and the component is freed.  This call may asynchronously free
  // system resources, but its client-visible effects are synchronous. After
  // this method returns no more callbacks will be made on the client. Deletes
  // |this| unconditionally, so make sure to drop all pointers to it!
  virtual void Destroy() = 0;

  // Flushes the encoder: all pending inputs will be encoded and all bitstreams
  // handed back to the client, and afterwards the |flush_callback| will be
  // called. The FlushCallback takes a boolean argument: |true| indicates the
  // flush is complete; |false| indicates the flush is cancelled due to errors
  // or destruction. The client should not invoke Flush() or Encode() while the
  // previous Flush() is not finished yet.
  virtual void Flush(FlushCallback flush_callback);

  // Returns true if the encoder support flush. This method must be called after
  // VEA has been initialized.
  virtual bool IsFlushSupported();

 protected:
  // Do not delete directly; use Destroy() or own it with a scoped_ptr, which
  // will Destroy() it properly by default.
  virtual ~VideoEncodeAccelerator();
};

}  // namespace media

namespace std {

// Specialize std::default_delete so that
// std::unique_ptr<VideoEncodeAccelerator> uses "Destroy()" instead of trying to
// use the destructor.
template <>
struct MEDIA_EXPORT default_delete<media::VideoEncodeAccelerator> {
  void operator()(media::VideoEncodeAccelerator* vea) const;
};

}  // namespace std

#endif  // MEDIA_VIDEO_VIDEO_ENCODE_ACCELERATOR_H_