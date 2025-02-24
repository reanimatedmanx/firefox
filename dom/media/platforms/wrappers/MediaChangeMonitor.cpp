/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaChangeMonitor.h"

#include "AnnexB.h"
#include "H264.h"
#include "H265.h"
#include "GeckoProfiler.h"
#include "ImageContainer.h"
#include "MP4Decoder.h"
#include "MediaInfo.h"
#include "PDMFactory.h"
#include "VPXDecoder.h"
#ifdef MOZ_AV1
#  include "AOMDecoder.h"
#endif
#include "gfxUtils.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/TaskQueue.h"

namespace mozilla {

extern LazyLogModule gMediaDecoderLog;

#define LOG(x, ...) \
  MOZ_LOG(gMediaDecoderLog, LogLevel::Debug, (x, ##__VA_ARGS__))

// H264ChangeMonitor is used to ensure that only AVCC or AnnexB is fed to the
// underlying MediaDataDecoder. The H264ChangeMonitor allows playback of content
// where the SPS NAL may not be provided in the init segment (e.g. AVC3 or Annex
// B) H264ChangeMonitor will monitor the input data, and will delay creation of
// the MediaDataDecoder until a SPS and PPS NALs have been extracted.

class H264ChangeMonitor : public MediaChangeMonitor::CodecChangeMonitor {
 public:
  explicit H264ChangeMonitor(const VideoInfo& aInfo, bool aFullParsing)
      : mCurrentConfig(aInfo), mFullParsing(aFullParsing) {
    if (CanBeInstantiated()) {
      UpdateConfigFromExtraData(aInfo.mExtraData);
    }
  }

  bool CanBeInstantiated() const override {
    return H264::HasSPS(mCurrentConfig.mExtraData);
  }

  MediaResult CheckForChange(MediaRawData* aSample) override {
    // To be usable we need to convert the sample to 4 bytes NAL size AVCC.
    if (!AnnexB::ConvertSampleToAVCC(aSample)) {
      // We need AVCC content to be able to later parse the SPS.
      // This is a no-op if the data is already AVCC.
      return MediaResult(NS_ERROR_OUT_OF_MEMORY,
                         RESULT_DETAIL("ConvertSampleToAVCC"));
    }

    if (!AnnexB::IsAVCC(aSample)) {
      return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                         RESULT_DETAIL("Invalid H264 content"));
    }

    RefPtr<MediaByteBuffer> extra_data =
        aSample->mKeyframe || !mGotSPS || mFullParsing
            ? H264::ExtractExtraData(aSample)
            : nullptr;

    if (!H264::HasSPS(extra_data) && !H264::HasSPS(mCurrentConfig.mExtraData)) {
      // We don't have inband data and the original config didn't contain a SPS.
      // We can't decode this content.
      return NS_ERROR_NOT_INITIALIZED;
    }

    mGotSPS = true;

    if (!H264::HasSPS(extra_data)) {
      // This sample doesn't contain inband SPS/PPS
      // We now check if the out of band one has changed.
      // This scenario can currently only occur on Android with devices that can
      // recycle a decoder.
      bool hasOutOfBandExtraData = H264::HasSPS(aSample->mExtraData);
      if (!hasOutOfBandExtraData || !mPreviousExtraData ||
          H264::CompareExtraData(aSample->mExtraData, mPreviousExtraData)) {
        if (hasOutOfBandExtraData && !mPreviousExtraData) {
          // We are decoding the first sample, store the out of band sample's
          // extradata so that we can check for future change.
          mPreviousExtraData = aSample->mExtraData;
        }
        return NS_OK;
      }
      extra_data = aSample->mExtraData;
    } else if (H264::CompareExtraData(extra_data, mCurrentConfig.mExtraData)) {
      return NS_OK;
    }

    // Store the sample's extradata so we don't trigger a false positive
    // with the out of band test on the next sample.
    mPreviousExtraData = aSample->mExtraData;
    UpdateConfigFromExtraData(extra_data);

    PROFILER_MARKER_TEXT("H264 Stream Change", MEDIA_PLAYBACK, {},
                         "H264ChangeMonitor::CheckForChange has detected a "
                         "change in the stream and will request a new decoder");
    return NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER;
  }

  const TrackInfo& Config() const override { return mCurrentConfig; }

  MediaResult PrepareSample(MediaDataDecoder::ConversionRequired aConversion,
                            MediaRawData* aSample,
                            bool aNeedKeyFrame) override {
    MOZ_DIAGNOSTIC_ASSERT(
        aConversion == MediaDataDecoder::ConversionRequired::kNeedAnnexB ||
            aConversion == MediaDataDecoder::ConversionRequired::kNeedAVCC,
        "Conversion must be either AVCC or AnnexB");

    aSample->mExtraData = mCurrentConfig.mExtraData;
    aSample->mTrackInfo = mTrackInfo;

    if (aConversion == MediaDataDecoder::ConversionRequired::kNeedAnnexB) {
      auto res = AnnexB::ConvertAVCCSampleToAnnexB(aSample, aNeedKeyFrame);
      if (res.isErr()) {
        return MediaResult(res.unwrapErr(),
                           RESULT_DETAIL("ConvertSampleToAnnexB"));
      }
    }

    return NS_OK;
  }

 private:
  void UpdateConfigFromExtraData(MediaByteBuffer* aExtraData) {
    SPSData spsdata;
    if (H264::DecodeSPSFromExtraData(aExtraData, spsdata) &&
        spsdata.pic_width > 0 && spsdata.pic_height > 0) {
      H264::EnsureSPSIsSane(spsdata);
      mCurrentConfig.mImage.width = spsdata.pic_width;
      mCurrentConfig.mImage.height = spsdata.pic_height;
      mCurrentConfig.mDisplay.width = spsdata.display_width;
      mCurrentConfig.mDisplay.height = spsdata.display_height;
      mCurrentConfig.mColorDepth = spsdata.ColorDepth();
      mCurrentConfig.mColorSpace = Some(spsdata.ColorSpace());
      // spsdata.colour_primaries has the same values as
      // gfx::CICP::ColourPrimaries.
      mCurrentConfig.mColorPrimaries = gfxUtils::CicpToColorPrimaries(
          static_cast<gfx::CICP::ColourPrimaries>(spsdata.colour_primaries),
          gMediaDecoderLog);
      // spsdata.transfer_characteristics has the same values as
      // gfx::CICP::TransferCharacteristics.
      mCurrentConfig.mTransferFunction = gfxUtils::CicpToTransferFunction(
          static_cast<gfx::CICP::TransferCharacteristics>(
              spsdata.transfer_characteristics));
      mCurrentConfig.mColorRange = spsdata.video_full_range_flag
                                       ? gfx::ColorRange::FULL
                                       : gfx::ColorRange::LIMITED;
    }
    mCurrentConfig.mExtraData = aExtraData;
    mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);
  }

  VideoInfo mCurrentConfig;
  uint32_t mStreamID = 0;
  const bool mFullParsing;
  bool mGotSPS = false;
  RefPtr<TrackInfoSharedPtr> mTrackInfo;
  RefPtr<MediaByteBuffer> mPreviousExtraData;
};

class HEVCChangeMonitor : public MediaChangeMonitor::CodecChangeMonitor {
 public:
  explicit HEVCChangeMonitor(const VideoInfo& aInfo) : mCurrentConfig(aInfo) {
    const bool canBeInstantiated = CanBeInstantiated();
    if (canBeInstantiated) {
      UpdateConfigFromExtraData(aInfo.mExtraData);
    }
    LOG("created HEVCChangeMonitor, CanBeInstantiated=%d", canBeInstantiated);
  }

  bool CanBeInstantiated() const override {
    auto rv = HVCCConfig::Parse(mCurrentConfig.mExtraData);
    if (rv.isErr()) {
      return false;
    }
    return rv.unwrap().HasSPS();
  }

  MediaResult CheckForChange(MediaRawData* aSample) override {
    // TODO : detect config change in bug 1852333.
    return NS_OK;
  }

  const TrackInfo& Config() const override { return mCurrentConfig; }

  MediaResult PrepareSample(MediaDataDecoder::ConversionRequired aConversion,
                            MediaRawData* aSample,
                            bool aNeedKeyFrame) override {
    MOZ_DIAGNOSTIC_ASSERT(aConversion ==
                          MediaDataDecoder::ConversionRequired::kNeedAnnexB);

    aSample->mExtraData = mCurrentConfig.mExtraData;
    aSample->mTrackInfo = mTrackInfo;

    if (AnnexB::IsHVCC(aSample)) {
      auto res = AnnexB::ConvertHVCCSampleToAnnexB(aSample, aNeedKeyFrame);
      if (res.isErr()) {
        return MediaResult(res.unwrapErr(),
                           RESULT_DETAIL("ConvertSampleToAnnexB"));
      }
    }
    return NS_OK;
  }

 private:
  void UpdateConfigFromExtraData(MediaByteBuffer* aExtraData) {
    if (auto rv = H265::DecodeSPSFromHVCCExtraData(aExtraData); rv.isOk()) {
      const auto sps = rv.unwrap();
      mCurrentConfig.mImage.width = sps.GetImageSize().Width();
      mCurrentConfig.mImage.height = sps.GetImageSize().Height();
      mCurrentConfig.mDisplay.width = sps.GetDisplaySize().Width();
      mCurrentConfig.mDisplay.height = sps.GetDisplaySize().Height();
      mCurrentConfig.mColorDepth = sps.ColorDepth();
      mCurrentConfig.mColorSpace = Some(sps.ColorSpace());
      mCurrentConfig.mColorPrimaries = gfxUtils::CicpToColorPrimaries(
          static_cast<gfx::CICP::ColourPrimaries>(sps.ColorPrimaries()),
          gMediaDecoderLog);
      mCurrentConfig.mTransferFunction = gfxUtils::CicpToTransferFunction(
          static_cast<gfx::CICP::TransferCharacteristics>(
              sps.TransferFunction()));
      mCurrentConfig.mColorRange = sps.IsFullColorRange()
                                       ? gfx::ColorRange::FULL
                                       : gfx::ColorRange::LIMITED;
    }
    mCurrentConfig.mExtraData = aExtraData;
    mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);
  }

  VideoInfo mCurrentConfig;
  uint32_t mStreamID = 0;
  RefPtr<TrackInfoSharedPtr> mTrackInfo;
};

// Gets the pixel aspect ratio from the decoded video size and the rendered
// size.
inline double GetPixelAspectRatio(const gfx::IntSize& aImage,
                                  const gfx::IntSize& aDisplay) {
  return (static_cast<double>(aDisplay.Width()) / aImage.Width()) /
         (static_cast<double>(aDisplay.Height()) / aImage.Height());
}

// Returns the render size based on the PAR and the new image size.
inline gfx::IntSize ApplyPixelAspectRatio(double aPixelAspectRatio,
                                          const gfx::IntSize& aImage) {
  return gfx::IntSize(static_cast<int32_t>(aImage.Width() * aPixelAspectRatio),
                      aImage.Height());
}

class VPXChangeMonitor : public MediaChangeMonitor::CodecChangeMonitor {
 public:
  explicit VPXChangeMonitor(const VideoInfo& aInfo)
      : mCurrentConfig(aInfo),
        mCodec(VPXDecoder::IsVP8(aInfo.mMimeType) ? VPXDecoder::Codec::VP8
                                                  : VPXDecoder::Codec::VP9),
        mPixelAspectRatio(GetPixelAspectRatio(aInfo.mImage, aInfo.mDisplay)) {
    mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);

    if (mCurrentConfig.mExtraData && !mCurrentConfig.mExtraData->IsEmpty()) {
      // If we're passed VP codec configuration, store it so that we can
      // instantiate the decoder on init.
      VPXDecoder::VPXStreamInfo vpxInfo;
      vpxInfo.mImage = mCurrentConfig.mImage;
      vpxInfo.mDisplay = mCurrentConfig.mDisplay;
      VPXDecoder::ReadVPCCBox(vpxInfo, mCurrentConfig.mExtraData);
      mInfo = Some(vpxInfo);
    }
  }

  bool CanBeInstantiated() const override {
    // We want to see at least one sample before we create a decoder so that we
    // can create the vpcC content on mCurrentConfig.mExtraData.
    return mCodec == VPXDecoder::Codec::VP8 || mInfo ||
           mCurrentConfig.mCrypto.IsEncrypted();
  }

  MediaResult CheckForChange(MediaRawData* aSample) override {
    // Don't look at encrypted content.
    if (aSample->mCrypto.IsEncrypted()) {
      return NS_OK;
    }
    auto dataSpan = Span<const uint8_t>(aSample->Data(), aSample->Size());

    // We don't trust the keyframe flag as set on the MediaRawData.
    VPXDecoder::VPXStreamInfo info;
    if (!VPXDecoder::GetStreamInfo(dataSpan, info, mCodec)) {
      return NS_ERROR_DOM_MEDIA_DECODE_ERR;
    }
    // For both VP8 and VP9, we only look for resolution changes
    // on keyframes. Other resolution changes are invalid.
    if (!info.mKeyFrame) {
      return NS_OK;
    }

    nsresult rv = NS_OK;
    if (mInfo) {
      if (mInfo.ref().IsCompatible(info)) {
        return rv;
      }
      // We can't properly determine the image rect once we've had a resolution
      // change.
      mCurrentConfig.ResetImageRect();
      PROFILER_MARKER_TEXT(
          "VPX Stream Change", MEDIA_PLAYBACK, {},
          "VPXChangeMonitor::CheckForChange has detected a change in the "
          "stream and will request a new decoder");
      rv = NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER;
    } else if (mCurrentConfig.mImage != info.mImage ||
               mCurrentConfig.mDisplay != info.mDisplay) {
      // We can't properly determine the image rect if we're changing
      // resolution based on sample information.
      mCurrentConfig.ResetImageRect();
      PROFILER_MARKER_TEXT("VPX Stream Init Discrepancy", MEDIA_PLAYBACK, {},
                           "VPXChangeMonitor::CheckForChange has detected a "
                           "discrepancy between initialization data and stream "
                           "content and will request a new decoder");
      rv = NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER;
    }

    LOG("Detect inband %s resolution changes, image (%" PRId32 ",%" PRId32
        ")->(%" PRId32 ",%" PRId32 "), display (%" PRId32 ",%" PRId32
        ")->(%" PRId32 ",%" PRId32 " %s)",
        mCodec == VPXDecoder::Codec::VP9 ? "VP9" : "VP8",
        mCurrentConfig.mImage.Width(), mCurrentConfig.mImage.Height(),
        info.mImage.Width(), info.mImage.Height(),
        mCurrentConfig.mDisplay.Width(), mCurrentConfig.mDisplay.Height(),
        info.mDisplay.Width(), info.mDisplay.Height(),
        info.mDisplayAndImageDifferent ? "specified" : "unspecified");

    mInfo = Some(info);
    mCurrentConfig.mImage = info.mImage;
    if (info.mDisplayAndImageDifferent) {
      // If the flag to change the display size is set in the sequence, we
      // set our original values to begin rescaling according to the new values.
      mCurrentConfig.mDisplay = info.mDisplay;
      mPixelAspectRatio = GetPixelAspectRatio(info.mImage, info.mDisplay);
    } else {
      mCurrentConfig.mDisplay =
          ApplyPixelAspectRatio(mPixelAspectRatio, info.mImage);
    }

    mCurrentConfig.mColorDepth = gfx::ColorDepthForBitDepth(info.mBitDepth);
    mCurrentConfig.mColorSpace = Some(info.ColorSpace());
    // VPX bitstream doesn't specify color primaries.

    // We don't update the transfer function here, because VPX bitstream
    // doesn't specify the transfer function. Instead, we keep the transfer
    // function (if any) that was set in mCurrentConfig when we were created.
    // If a video changes colorspaces away from BT2020, we won't clear
    // mTransferFunction, in case the video changes back to BT2020 and we
    // need the value again.

    mCurrentConfig.mColorRange = info.ColorRange();
    if (mCodec == VPXDecoder::Codec::VP9) {
      mCurrentConfig.mExtraData->ClearAndRetainStorage();
      VPXDecoder::GetVPCCBox(mCurrentConfig.mExtraData, info);
    }
    mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);

    return rv;
  }

  const TrackInfo& Config() const override { return mCurrentConfig; }

  MediaResult PrepareSample(MediaDataDecoder::ConversionRequired aConversion,
                            MediaRawData* aSample,
                            bool aNeedKeyFrame) override {
    aSample->mTrackInfo = mTrackInfo;

    return NS_OK;
  }

 private:
  VideoInfo mCurrentConfig;
  const VPXDecoder::Codec mCodec;
  Maybe<VPXDecoder::VPXStreamInfo> mInfo;
  uint32_t mStreamID = 0;
  RefPtr<TrackInfoSharedPtr> mTrackInfo;
  double mPixelAspectRatio;
};

#ifdef MOZ_AV1
class AV1ChangeMonitor : public MediaChangeMonitor::CodecChangeMonitor {
 public:
  explicit AV1ChangeMonitor(const VideoInfo& aInfo)
      : mCurrentConfig(aInfo),
        mPixelAspectRatio(GetPixelAspectRatio(aInfo.mImage, aInfo.mDisplay)) {
    mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);

    if (mCurrentConfig.mExtraData && !mCurrentConfig.mExtraData->IsEmpty()) {
      // If we're passed AV1 codec configuration, store it so that we can
      // instantiate a decoder in MediaChangeMonitor::Create.
      AOMDecoder::AV1SequenceInfo seqInfo;
      MediaResult seqHdrResult;
      AOMDecoder::TryReadAV1CBox(mCurrentConfig.mExtraData, seqInfo,
                                 seqHdrResult);
      // If the av1C box doesn't include a sequence header specifying image
      // size, keep the one provided by VideoInfo.
      if (seqHdrResult.Code() != NS_OK) {
        seqInfo.mImage = mCurrentConfig.mImage;
      }

      UpdateConfig(seqInfo);
    }
  }

  bool CanBeInstantiated() const override {
    // We want to have enough codec configuration to determine whether hardware
    // decoding can be used before creating a decoder. The av1C box or a
    // sequence header from a sample will contain this information.
    return mInfo || mCurrentConfig.mCrypto.IsEncrypted();
  }

  void UpdateConfig(const AOMDecoder::AV1SequenceInfo& aInfo) {
    mInfo = Some(aInfo);
    mCurrentConfig.mColorDepth = gfx::ColorDepthForBitDepth(aInfo.mBitDepth);
    mCurrentConfig.mColorSpace = gfxUtils::CicpToColorSpace(
        aInfo.mColorSpace.mMatrix, aInfo.mColorSpace.mPrimaries,
        gMediaDecoderLog);
    mCurrentConfig.mColorPrimaries = gfxUtils::CicpToColorPrimaries(
        aInfo.mColorSpace.mPrimaries, gMediaDecoderLog);
    mCurrentConfig.mTransferFunction =
        gfxUtils::CicpToTransferFunction(aInfo.mColorSpace.mTransfer);
    mCurrentConfig.mColorRange = aInfo.mColorSpace.mRange;

    if (mCurrentConfig.mImage != mInfo->mImage) {
      gfx::IntSize newDisplay =
          ApplyPixelAspectRatio(mPixelAspectRatio, aInfo.mImage);
      LOG("AV1ChangeMonitor detected a resolution change in-band, image "
          "(%" PRIu32 ",%" PRIu32 ")->(%" PRIu32 ",%" PRIu32
          "), display (%" PRIu32 ",%" PRIu32 ")->(%" PRIu32 ",%" PRIu32
          " from PAR)",
          mCurrentConfig.mImage.Width(), mCurrentConfig.mImage.Height(),
          aInfo.mImage.Width(), aInfo.mImage.Height(),
          mCurrentConfig.mDisplay.Width(), mCurrentConfig.mDisplay.Height(),
          newDisplay.Width(), newDisplay.Height());
      mCurrentConfig.mImage = aInfo.mImage;
      mCurrentConfig.mDisplay = newDisplay;
      mCurrentConfig.ResetImageRect();
    }

    bool wroteSequenceHeader = false;
    // Our headers should all be around the same size.
    mCurrentConfig.mExtraData->ClearAndRetainStorage();
    AOMDecoder::WriteAV1CBox(aInfo, mCurrentConfig.mExtraData.get(),
                             wroteSequenceHeader);
    // Header should always be written ReadSequenceHeaderInfo succeeds.
    MOZ_ASSERT(wroteSequenceHeader);
  }

  MediaResult CheckForChange(MediaRawData* aSample) override {
    // Don't look at encrypted content.
    if (aSample->mCrypto.IsEncrypted()) {
      return NS_OK;
    }
    auto dataSpan = Span<const uint8_t>(aSample->Data(), aSample->Size());

    // We don't trust the keyframe flag as set on the MediaRawData.
    AOMDecoder::AV1SequenceInfo info;
    MediaResult seqHdrResult =
        AOMDecoder::ReadSequenceHeaderInfo(dataSpan, info);
    nsresult seqHdrCode = seqHdrResult.Code();
    if (seqHdrCode == NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA) {
      return NS_OK;
    }
    if (seqHdrCode != NS_OK) {
      LOG("AV1ChangeMonitor::CheckForChange read a corrupted sample: %s",
          seqHdrResult.Description().get());
      return seqHdrResult;
    }

    nsresult rv = NS_OK;
    if (mInfo.isSome() &&
        (mInfo->mProfile != info.mProfile ||
         mInfo->ColorDepth() != info.ColorDepth() ||
         mInfo->mMonochrome != info.mMonochrome ||
         mInfo->mSubsamplingX != info.mSubsamplingX ||
         mInfo->mSubsamplingY != info.mSubsamplingY ||
         mInfo->mChromaSamplePosition != info.mChromaSamplePosition ||
         mInfo->mImage != info.mImage)) {
      PROFILER_MARKER_TEXT(
          "AV1 Stream Change", MEDIA_PLAYBACK, {},
          "AV1ChangeMonitor::CheckForChange has detected a change in a "
          "stream and will request a new decoder");
      LOG("AV1ChangeMonitor detected a change and requests a new decoder");
      rv = NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER;
    }

    UpdateConfig(info);

    if (rv == NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER) {
      mTrackInfo = new TrackInfoSharedPtr(mCurrentConfig, mStreamID++);
    }
    return rv;
  }

  const TrackInfo& Config() const override { return mCurrentConfig; }

  MediaResult PrepareSample(MediaDataDecoder::ConversionRequired aConversion,
                            MediaRawData* aSample,
                            bool aNeedKeyFrame) override {
    aSample->mTrackInfo = mTrackInfo;
    return NS_OK;
  }

 private:
  VideoInfo mCurrentConfig;
  Maybe<AOMDecoder::AV1SequenceInfo> mInfo;
  uint32_t mStreamID = 0;
  RefPtr<TrackInfoSharedPtr> mTrackInfo;
  double mPixelAspectRatio;
};
#endif

MediaChangeMonitor::MediaChangeMonitor(
    PDMFactory* aPDMFactory,
    UniquePtr<CodecChangeMonitor>&& aCodecChangeMonitor,
    MediaDataDecoder* aDecoder, const CreateDecoderParams& aParams)
    : mChangeMonitor(std::move(aCodecChangeMonitor)),
      mPDMFactory(aPDMFactory),
      mCurrentConfig(aParams.VideoConfig()),
      mDecoder(aDecoder),
      mParams(aParams) {}

/* static */
RefPtr<PlatformDecoderModule::CreateDecoderPromise> MediaChangeMonitor::Create(
    PDMFactory* aPDMFactory, const CreateDecoderParams& aParams) {
  UniquePtr<CodecChangeMonitor> changeMonitor;
  const VideoInfo& currentConfig = aParams.VideoConfig();
  if (VPXDecoder::IsVPX(currentConfig.mMimeType)) {
    changeMonitor = MakeUnique<VPXChangeMonitor>(currentConfig);
#ifdef MOZ_AV1
  } else if (AOMDecoder::IsAV1(currentConfig.mMimeType)) {
    changeMonitor = MakeUnique<AV1ChangeMonitor>(currentConfig);
#endif
  } else if (MP4Decoder::IsHEVC(currentConfig.mMimeType)) {
    changeMonitor = MakeUnique<HEVCChangeMonitor>(currentConfig);
  } else {
    MOZ_ASSERT(MP4Decoder::IsH264(currentConfig.mMimeType));
    changeMonitor = MakeUnique<H264ChangeMonitor>(
        currentConfig, aParams.mOptions.contains(
                           CreateDecoderParams::Option::FullH264Parsing));
  }

  // The change monitor may have an updated track config. E.g. the h264 monitor
  // may update the config after parsing extra data in the VideoInfo. Create a
  // new set of params with the updated track info from our monitor and the
  // other params for aParams and use that going forward.
  const CreateDecoderParams updatedParams{changeMonitor->Config(), aParams};

  RefPtr<MediaChangeMonitor> instance = new MediaChangeMonitor(
      aPDMFactory, std::move(changeMonitor), nullptr, updatedParams);

  if (instance->mChangeMonitor->CanBeInstantiated()) {
    RefPtr<PlatformDecoderModule::CreateDecoderPromise> p =
        instance->CreateDecoder()->Then(
            GetCurrentSerialEventTarget(), __func__,
            [instance = RefPtr{instance}] {
              return PlatformDecoderModule::CreateDecoderPromise::
                  CreateAndResolve(instance, __func__);
            },
            [](const MediaResult& aError) {
              return PlatformDecoderModule::CreateDecoderPromise::
                  CreateAndReject(aError, __func__);
            });
    return p;
  }

  return PlatformDecoderModule::CreateDecoderPromise::CreateAndResolve(
      instance, __func__);
}

MediaChangeMonitor::~MediaChangeMonitor() = default;

RefPtr<MediaDataDecoder::InitPromise> MediaChangeMonitor::Init() {
  mThread = GetCurrentSerialEventTarget();
  if (mDecoder) {
    RefPtr<InitPromise> p = mInitPromise.Ensure(__func__);
    RefPtr<MediaChangeMonitor> self = this;
    mDecoder->Init()
        ->Then(GetCurrentSerialEventTarget(), __func__,
               [self, this](InitPromise::ResolveOrRejectValue&& aValue) {
                 mInitPromiseRequest.Complete();
                 if (aValue.IsResolve()) {
                   mDecoderInitialized = true;
                   mConversionRequired = Some(mDecoder->NeedsConversion());
                   mCanRecycleDecoder = Some(CanRecycleDecoder());
                 }
                 return mInitPromise.ResolveOrRejectIfExists(std::move(aValue),
                                                             __func__);
               })
        ->Track(mInitPromiseRequest);
    return p;
  }

  // We haven't been able to initialize a decoder due to missing
  // extradata.
  return MediaDataDecoder::InitPromise::CreateAndResolve(TrackType::kVideoTrack,
                                                         __func__);
}

RefPtr<MediaDataDecoder::DecodePromise> MediaChangeMonitor::Decode(
    MediaRawData* aSample) {
  AssertOnThread();
  MOZ_RELEASE_ASSERT(mFlushPromise.IsEmpty(),
                     "Flush operation didn't complete");

  MOZ_RELEASE_ASSERT(
      !mDecodePromiseRequest.Exists() && !mInitPromiseRequest.Exists(),
      "Can't request a new decode until previous one completed");

  MediaResult rv = CheckForChange(aSample);

  if (rv == NS_ERROR_NOT_INITIALIZED) {
    // We are missing the required init data to create the decoder.
    if (mParams.mOptions.contains(
            CreateDecoderParams::Option::ErrorIfNoInitializationData)) {
      // This frame can't be decoded and should be treated as an error.
      return DecodePromise::CreateAndReject(rv, __func__);
    }
    // Swallow the frame, and await delivery of init data.
    return DecodePromise::CreateAndResolve(DecodedData(), __func__);
  }
  if (rv == NS_ERROR_DOM_MEDIA_INITIALIZING_DECODER) {
    // The decoder is pending initialization.
    RefPtr<DecodePromise> p = mDecodePromise.Ensure(__func__);
    return p;
  }

  if (NS_FAILED(rv)) {
    return DecodePromise::CreateAndReject(rv, __func__);
  }

  if (mNeedKeyframe && !aSample->mKeyframe) {
    return DecodePromise::CreateAndResolve(DecodedData(), __func__);
  }

  rv = mChangeMonitor->PrepareSample(*mConversionRequired, aSample,
                                     mNeedKeyframe);
  if (NS_FAILED(rv)) {
    return DecodePromise::CreateAndReject(rv, __func__);
  }

  mNeedKeyframe = false;

  return mDecoder->Decode(aSample);
}

RefPtr<MediaDataDecoder::FlushPromise> MediaChangeMonitor::Flush() {
  AssertOnThread();
  mDecodePromiseRequest.DisconnectIfExists();
  mDecodePromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mNeedKeyframe = true;
  mPendingFrames.Clear();

  MOZ_RELEASE_ASSERT(mFlushPromise.IsEmpty(), "Previous flush didn't complete");

  /*
    When we detect a change of content in the byte stream, we first drain the
    current decoder (1), flush (2), shut it down (3) create a new decoder (4)
    and initialize it (5). It is possible for MediaChangeMonitor::Flush to be
    called during any of those times. If during (1):
      - mDrainRequest will not be empty.
      - The old decoder can still be used, with the current extradata as
    stored in mCurrentConfig.mExtraData.

    If during (2):
      - mFlushRequest will not be empty.
      - The old decoder can still be used, with the current extradata as
    stored in mCurrentConfig.mExtraData.

    If during (3):
      - mShutdownRequest won't be empty.
      - mDecoder is empty.
      - The old decoder is no longer referenced by the MediaChangeMonitor.

    If during (4):
      - mDecoderRequest won't be empty.
      - mDecoder is not set. Steps will continue to (5) to set and initialize it

    If during (5):
      - mInitPromiseRequest won't be empty.
      - mDecoder is set but not usable yet.
  */

  if (mDrainRequest.Exists() || mFlushRequest.Exists() ||
      mShutdownRequest.Exists() || mDecoderRequest.Exists() ||
      mInitPromiseRequest.Exists()) {
    // We let the current decoder complete and will resume after.
    RefPtr<FlushPromise> p = mFlushPromise.Ensure(__func__);
    return p;
  }
  if (mDecoder && mDecoderInitialized) {
    return mDecoder->Flush();
  }
  return FlushPromise::CreateAndResolve(true, __func__);
}

RefPtr<MediaDataDecoder::DecodePromise> MediaChangeMonitor::Drain() {
  AssertOnThread();
  MOZ_RELEASE_ASSERT(!mDrainRequest.Exists());
  mNeedKeyframe = true;
  if (mDecoder) {
    return mDecoder->Drain();
  }
  return DecodePromise::CreateAndResolve(DecodedData(), __func__);
}

RefPtr<ShutdownPromise> MediaChangeMonitor::Shutdown() {
  AssertOnThread();
  mInitPromiseRequest.DisconnectIfExists();
  mInitPromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mDecodePromiseRequest.DisconnectIfExists();
  mDecodePromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mDrainRequest.DisconnectIfExists();
  mFlushRequest.DisconnectIfExists();
  mFlushPromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mShutdownRequest.DisconnectIfExists();

  if (mShutdownPromise) {
    // We have a shutdown in progress, return that promise instead as we can't
    // shutdown a decoder twice.
    RefPtr<ShutdownPromise> p = std::move(mShutdownPromise);
    return p;
  }
  return ShutdownDecoder();
}

RefPtr<ShutdownPromise> MediaChangeMonitor::ShutdownDecoder() {
  AssertOnThread();
  mConversionRequired.reset();
  if (mDecoder) {
    RefPtr<MediaDataDecoder> decoder = std::move(mDecoder);
    return decoder->Shutdown();
  }
  return ShutdownPromise::CreateAndResolve(true, __func__);
}

bool MediaChangeMonitor::IsHardwareAccelerated(
    nsACString& aFailureReason) const {
  if (mDecoder) {
    return mDecoder->IsHardwareAccelerated(aFailureReason);
  }
#ifdef MOZ_APPLEMEDIA
  // On mac, we can assume H264 is hardware accelerated for now.
  // This allows MediaCapabilities to report that playback will be smooth.
  // Which will always be.
  return true;
#else
  return MediaDataDecoder::IsHardwareAccelerated(aFailureReason);
#endif
}

void MediaChangeMonitor::SetSeekThreshold(const media::TimeUnit& aTime) {
  if (mDecoder) {
    mDecoder->SetSeekThreshold(aTime);
  } else {
    MediaDataDecoder::SetSeekThreshold(aTime);
  }
}

RefPtr<MediaChangeMonitor::CreateDecoderPromise>
MediaChangeMonitor::CreateDecoder() {
  mCurrentConfig = *mChangeMonitor->Config().GetAsVideoInfo();
  RefPtr<CreateDecoderPromise> p =
      mPDMFactory
          ->CreateDecoder(
              {mCurrentConfig, mParams, CreateDecoderParams::NoWrapper(true)})
          ->Then(
              GetCurrentSerialEventTarget(), __func__,
              [self = RefPtr{this}, this](RefPtr<MediaDataDecoder>&& aDecoder) {
                mDecoder = std::move(aDecoder);
                DDLINKCHILD("decoder", mDecoder.get());
                return CreateDecoderPromise::CreateAndResolve(true, __func__);
              },
              [self = RefPtr{this}](const MediaResult& aError) {
                return CreateDecoderPromise::CreateAndReject(aError, __func__);
              });

  mDecoderInitialized = false;
  mNeedKeyframe = true;

  return p;
}

MediaResult MediaChangeMonitor::CreateDecoderAndInit(MediaRawData* aSample) {
  MOZ_ASSERT(mThread && mThread->IsOnCurrentThread());

  MediaResult rv = mChangeMonitor->CheckForChange(aSample);
  if (!NS_SUCCEEDED(rv) && rv != NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER) {
    return rv;
  }

  if (!mChangeMonitor->CanBeInstantiated()) {
    // Nothing found yet, will try again later.
    return NS_ERROR_NOT_INITIALIZED;
  }

  CreateDecoder()
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, this, sample = RefPtr{aSample}] {
            mDecoderRequest.Complete();
            mDecoder->Init()
                ->Then(
                    GetCurrentSerialEventTarget(), __func__,
                    [self, sample, this](const TrackType aTrackType) {
                      mInitPromiseRequest.Complete();
                      mDecoderInitialized = true;
                      mConversionRequired = Some(mDecoder->NeedsConversion());
                      mCanRecycleDecoder = Some(CanRecycleDecoder());

                      if (!mFlushPromise.IsEmpty()) {
                        // A Flush is pending, abort the current operation.
                        mFlushPromise.Resolve(true, __func__);
                        return;
                      }

                      DecodeFirstSample(sample);
                    },
                    [self, this](const MediaResult& aError) {
                      mInitPromiseRequest.Complete();

                      if (!mFlushPromise.IsEmpty()) {
                        // A Flush is pending, abort the current operation.
                        mFlushPromise.Reject(aError, __func__);
                        return;
                      }

                      mDecodePromise.Reject(
                          MediaResult(
                              aError.Code(),
                              RESULT_DETAIL("Unable to initialize decoder")),
                          __func__);
                    })
                ->Track(mInitPromiseRequest);
          },
          [self = RefPtr{this}, this](const MediaResult& aError) {
            mDecoderRequest.Complete();
            if (!mFlushPromise.IsEmpty()) {
              // A Flush is pending, abort the current operation.
              mFlushPromise.Reject(aError, __func__);
              return;
            }
            mDecodePromise.Reject(
                MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                            RESULT_DETAIL("Unable to create decoder")),
                __func__);
          })
      ->Track(mDecoderRequest);
  return NS_ERROR_DOM_MEDIA_INITIALIZING_DECODER;
}

bool MediaChangeMonitor::CanRecycleDecoder() const {
  MOZ_ASSERT(mDecoder);
  return StaticPrefs::media_decoder_recycle_enabled() &&
         mDecoder->SupportDecoderRecycling();
}

void MediaChangeMonitor::DecodeFirstSample(MediaRawData* aSample) {
  // We feed all the data to AnnexB decoder as a non-keyframe could contain
  // the SPS/PPS when used with WebRTC and this data is needed by the decoder.
  if (mNeedKeyframe && !aSample->mKeyframe &&
      *mConversionRequired != ConversionRequired::kNeedAnnexB) {
    mDecodePromise.Resolve(std::move(mPendingFrames), __func__);
    mPendingFrames = DecodedData();
    return;
  }

  MediaResult rv = mChangeMonitor->PrepareSample(*mConversionRequired, aSample,
                                                 mNeedKeyframe);

  if (NS_FAILED(rv)) {
    mDecodePromise.Reject(rv, __func__);
    return;
  }

  if (aSample->mKeyframe) {
    mNeedKeyframe = false;
  }

  RefPtr<MediaChangeMonitor> self = this;
  mDecoder->Decode(aSample)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self, this](MediaDataDecoder::DecodedData&& aResults) {
            mDecodePromiseRequest.Complete();
            mPendingFrames.AppendElements(std::move(aResults));
            mDecodePromise.Resolve(std::move(mPendingFrames), __func__);
            mPendingFrames = DecodedData();
          },
          [self, this](const MediaResult& aError) {
            mDecodePromiseRequest.Complete();
            mDecodePromise.Reject(aError, __func__);
          })
      ->Track(mDecodePromiseRequest);
}

MediaResult MediaChangeMonitor::CheckForChange(MediaRawData* aSample) {
  if (!mDecoder) {
    return CreateDecoderAndInit(aSample);
  }

  MediaResult rv = mChangeMonitor->CheckForChange(aSample);

  if (NS_SUCCEEDED(rv) || rv != NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER) {
    return rv;
  }

  if (*mCanRecycleDecoder) {
    // Do not recreate the decoder, reuse it.
    mNeedKeyframe = true;
    return NS_OK;
  }

  // The content has changed, signal to drain the current decoder and once done
  // create a new one.
  DrainThenFlushDecoder(aSample);
  return NS_ERROR_DOM_MEDIA_INITIALIZING_DECODER;
}

void MediaChangeMonitor::DrainThenFlushDecoder(MediaRawData* aPendingSample) {
  AssertOnThread();
  MOZ_ASSERT(mDecoderInitialized);
  RefPtr<MediaRawData> sample = aPendingSample;
  RefPtr<MediaChangeMonitor> self = this;
  mDecoder->Drain()
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self, sample, this](MediaDataDecoder::DecodedData&& aResults) {
            mDrainRequest.Complete();
            if (!mFlushPromise.IsEmpty()) {
              // A Flush is pending, abort the current operation.
              mFlushPromise.Resolve(true, __func__);
              return;
            }
            if (aResults.Length() > 0) {
              mPendingFrames.AppendElements(std::move(aResults));
              DrainThenFlushDecoder(sample);
              return;
            }
            // We've completed the draining operation, we can now flush the
            // decoder.
            FlushThenShutdownDecoder(sample);
          },
          [self, this](const MediaResult& aError) {
            mDrainRequest.Complete();
            if (!mFlushPromise.IsEmpty()) {
              // A Flush is pending, abort the current operation.
              mFlushPromise.Reject(aError, __func__);
              return;
            }
            mDecodePromise.Reject(aError, __func__);
          })
      ->Track(mDrainRequest);
}

void MediaChangeMonitor::FlushThenShutdownDecoder(
    MediaRawData* aPendingSample) {
  AssertOnThread();
  MOZ_ASSERT(mDecoderInitialized);
  RefPtr<MediaRawData> sample = aPendingSample;
  RefPtr<MediaChangeMonitor> self = this;
  mDecoder->Flush()
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self, sample, this]() {
            mFlushRequest.Complete();

            if (!mFlushPromise.IsEmpty()) {
              // A Flush is pending, abort the current operation.
              mFlushPromise.Resolve(true, __func__);
              return;
            }

            mShutdownPromise = ShutdownDecoder();
            mShutdownPromise
                ->Then(
                    GetCurrentSerialEventTarget(), __func__,
                    [self, sample, this]() {
                      mShutdownRequest.Complete();
                      mShutdownPromise = nullptr;

                      if (!mFlushPromise.IsEmpty()) {
                        // A Flush is pending, abort the current
                        // operation.
                        mFlushPromise.Resolve(true, __func__);
                        return;
                      }

                      MediaResult rv = CreateDecoderAndInit(sample);
                      if (rv == NS_ERROR_DOM_MEDIA_INITIALIZING_DECODER) {
                        // All good so far, will continue later.
                        return;
                      }
                      MOZ_ASSERT(NS_FAILED(rv));
                      mDecodePromise.Reject(rv, __func__);
                      return;
                    },
                    [] { MOZ_CRASH("Can't reach here'"); })
                ->Track(mShutdownRequest);
          },
          [self, this](const MediaResult& aError) {
            mFlushRequest.Complete();
            if (!mFlushPromise.IsEmpty()) {
              // A Flush is pending, abort the current operation.
              mFlushPromise.Reject(aError, __func__);
              return;
            }
            mDecodePromise.Reject(aError, __func__);
          })
      ->Track(mFlushRequest);
}

#undef LOG

}  // namespace mozilla
