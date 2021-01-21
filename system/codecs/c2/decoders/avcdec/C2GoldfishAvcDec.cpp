/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2GoldfishAvcDec"
#include <inttypes.h>
#include <log/log.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
//#include <android/hardware/graphics/common/1.0/types.h>

#include <android/hardware/graphics/allocator/3.0/IAllocator.h>
#include <android/hardware/graphics/mapper/3.0/IMapper.h>
#include <hidl/LegacySupport.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <SimpleC2Interface.h>
#include <goldfish_codec2/store/GoldfishComponentStore.h>
#include <gralloc_cb_bp.h>

#include "C2GoldfishAvcDec.h"

#define DEBUG 1
#if DEBUG
#define DDD(...) ALOGD(__VA_ARGS__)
#else
#define DDD(...) ((void)0)
#endif

using ::android::hardware::graphics::common::V1_0::BufferUsage;
using ::android::hardware::graphics::common::V1_2::PixelFormat;

namespace android {

namespace {
constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;
constexpr char COMPONENT_NAME[] = "c2.goldfish.h264.decoder";
constexpr uint32_t kDefaultOutputDelay = 8;
/* avc specification allows for a maximum delay of 16 frames.
   As soft avc decoder supports interlaced, this delay would be 32 fields.
   And avc decoder implementation has an additional delay of 2 decode calls.
   So total maximum output delay is 34 */
constexpr uint32_t kMaxOutputDelay = 34;
constexpr uint32_t kMinInputBytes = 4;
} // namespace

class C2GoldfishAvcDec::IntfImpl : public SimpleInterface<void>::BaseParams {
  public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : SimpleInterface<void>::BaseParams(
              helper, COMPONENT_NAME, C2Component::KIND_DECODER,
              C2Component::DOMAIN_VIDEO, MEDIA_MIMETYPE_VIDEO_AVC) {
        noPrivateBuffers(); // TODO: account for our buffers here
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();

        // TODO: Proper support for reorder depth.
        addParameter(
            DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(
                    new C2PortActualDelayTuning::output(kDefaultOutputDelay))
                .withFields({C2F(mActualOutputDelay, value)
                                 .inRange(0, kMaxOutputDelay)})
                .withSetter(
                    Setter<
                        decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        // TODO: output latency and reordering

        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                         .withConstValue(new C2ComponentAttributesSetting(
                             C2Component::ATTRIB_IS_TEMPORAL))
                         .build());

        // coded and output picture size is the same for this codec
        addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, 4080, 2),
                    C2F(mSize, height).inRange(2, 4080, 2),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                         .withDefault(new C2StreamMaxPictureSizeTuning::output(
                             0u, 320, 240))
                         .withFields({
                             C2F(mSize, width).inRange(2, 4080, 2),
                             C2F(mSize, height).inRange(2, 4080, 2),
                         })
                         .withSetter(MaxPictureSizeSetter, mSize)
                         .build());

        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                    C2Config::LEVEL_AVC_5_2))
                .withFields(
                    {C2F(mProfileLevel, profile)
                         .oneOf({C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                                 C2Config::PROFILE_AVC_BASELINE,
                                 C2Config::PROFILE_AVC_MAIN,
                                 C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                                 C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
                                 C2Config::PROFILE_AVC_HIGH}),
                     C2F(mProfileLevel, level)
                         .oneOf(
                             {C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B,
                              C2Config::LEVEL_AVC_1_1, C2Config::LEVEL_AVC_1_2,
                              C2Config::LEVEL_AVC_1_3, C2Config::LEVEL_AVC_2,
                              C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                              C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1,
                              C2Config::LEVEL_AVC_3_2, C2Config::LEVEL_AVC_4,
                              C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                              C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1,
                              C2Config::LEVEL_AVC_5_2})})
                .withSetter(ProfileLevelSetter, mSize)
                .build());

        addParameter(
            DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withDefault(new C2StreamMaxBufferSizeInfo::input(
                    0u, kMinInputBufferSize))
                .withFields({
                    C2F(mMaxInputSize, value).any(),
                })
                .calculatedAs(MaxInputSizeSetter, mMaxSize)
                .build());

        C2ChromaOffsetStruct locations[1] = {
            C2ChromaOffsetStruct::ITU_YUV_420_0()};
        std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
            C2StreamColorInfo::output::AllocShared(1u, 0u, 8u /* bitDepth */,
                                                   C2Color::YUV_420);
        memcpy(defaultColorInfo->m.locations, locations, sizeof(locations));

        defaultColorInfo = C2StreamColorInfo::output::AllocShared(
            {C2ChromaOffsetStruct::ITU_YUV_420_0()}, 0u, 8u /* bitDepth */,
            C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();

        addParameter(DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
                         .withConstValue(defaultColorInfo)
                         .build());

        addParameter(
            DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsTuning::output(
                    0u, C2Color::RANGE_UNSPECIFIED,
                    C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({C2F(mDefaultColorAspects, range)
                                 .inRange(C2Color::RANGE_UNSPECIFIED,
                                          C2Color::RANGE_OTHER),
                             C2F(mDefaultColorAspects, primaries)
                                 .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                                          C2Color::PRIMARIES_OTHER),
                             C2F(mDefaultColorAspects, transfer)
                                 .inRange(C2Color::TRANSFER_UNSPECIFIED,
                                          C2Color::TRANSFER_OTHER),
                             C2F(mDefaultColorAspects, matrix)
                                 .inRange(C2Color::MATRIX_UNSPECIFIED,
                                          C2Color::MATRIX_OTHER)})
                .withSetter(DefaultColorAspectsSetter)
                .build());

        addParameter(
            DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::input(
                    0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({C2F(mCodedColorAspects, range)
                                 .inRange(C2Color::RANGE_UNSPECIFIED,
                                          C2Color::RANGE_OTHER),
                             C2F(mCodedColorAspects, primaries)
                                 .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                                          C2Color::PRIMARIES_OTHER),
                             C2F(mCodedColorAspects, transfer)
                                 .inRange(C2Color::TRANSFER_UNSPECIFIED,
                                          C2Color::TRANSFER_OTHER),
                             C2F(mCodedColorAspects, matrix)
                                 .inRange(C2Color::MATRIX_UNSPECIFIED,
                                          C2Color::MATRIX_OTHER)})
                .withSetter(CodedColorAspectsSetter)
                .build());

        addParameter(
            DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::output(
                    0u, C2Color::RANGE_UNSPECIFIED,
                    C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({C2F(mColorAspects, range)
                                 .inRange(C2Color::RANGE_UNSPECIFIED,
                                          C2Color::RANGE_OTHER),
                             C2F(mColorAspects, primaries)
                                 .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                                          C2Color::PRIMARIES_OTHER),
                             C2F(mColorAspects, transfer)
                                 .inRange(C2Color::TRANSFER_UNSPECIFIED,
                                          C2Color::TRANSFER_OTHER),
                             C2F(mColorAspects, matrix)
                                 .inRange(C2Color::MATRIX_UNSPECIFIED,
                                          C2Color::MATRIX_OTHER)})
                .withSetter(ColorAspectsSetter, mDefaultColorAspects,
                            mCodedColorAspects)
                .build());

        // TODO: support more formats?
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                         .withConstValue(new C2StreamPixelFormatInfo::output(
                             0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                         .build());
    }
    static C2R SizeSetter(bool mayBlock,
                          const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                          C2P<C2StreamPictureSizeInfo::output> &me) {
        (void)mayBlock;
        DDD("calling sizesetter now %d", oldMe.v.height);
        DDD("new calling sizesetter now %d", me.v.height);

        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R
    MaxPictureSizeSetter(bool mayBlock,
                         C2P<C2StreamMaxPictureSizeTuning::output> &me,
                         const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        // TODO: get max width/height from the size's field helpers vs.
        // hardcoding
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), 4080u);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), 4080u);
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(
        bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
        const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        (void)mayBlock;
        // assume compression ratio of 2
        me.set().value = c2_max((((maxSize.v.width + 15) / 16) *
                                 ((maxSize.v.height + 15) / 16) * 192),
                                kMinInputBufferSize);
        return C2R::Ok();
    }

    static C2R
    ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me,
                       const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        (void)size;
        (void)me; // TODO: validate
        return C2R::Ok();
    }

    static C2R
    DefaultColorAspectsSetter(bool mayBlock,
                              C2P<C2StreamColorAspectsTuning::output> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
            me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
            me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
            me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
            me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R
    CodedColorAspectsSetter(bool mayBlock,
                            C2P<C2StreamColorAspectsInfo::input> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
            me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
            me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
            me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
            me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R
    ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                       const C2P<C2StreamColorAspectsTuning::output> &def,
                       const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        // take default values for all unspecified fields, and coded values for
        // specified ones
        me.set().range =
            coded.v.range == RANGE_UNSPECIFIED ? def.v.range : coded.v.range;
        me.set().primaries = coded.v.primaries == PRIMARIES_UNSPECIFIED
                                 ? def.v.primaries
                                 : coded.v.primaries;
        me.set().transfer = coded.v.transfer == TRANSFER_UNSPECIFIED
                                ? def.v.transfer
                                : coded.v.transfer;
        me.set().matrix = coded.v.matrix == MATRIX_UNSPECIFIED ? def.v.matrix
                                                               : coded.v.matrix;
        return C2R::Ok();
    }

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() {
        return mColorAspects;
    }

    int width() const { return mSize->width; }

    int height() const { return mSize->height; }

  private:
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
};

static void *ivd_aligned_malloc(void *ctxt, uint32_t alignment, uint32_t size) {
    (void)ctxt;
    return memalign(alignment, size);
}

static void ivd_aligned_free(void *ctxt, void *mem) {
    (void)ctxt;
    free(mem);
}

C2GoldfishAvcDec::C2GoldfishAvcDec(const char *name, c2_node_id_t id,
                                   const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(
          std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl), mOutBufferFlush(nullptr), mWidth(1920), mHeight(1080),
      mHeaderDecoded(false), mOutIndex(0u) {
    mWidth = mIntf->width();
    mHeight = mIntf->height();
    DDD("creating avc decoder now w %d h %d", mWidth, mHeight);
}

C2GoldfishAvcDec::~C2GoldfishAvcDec() { onRelease(); }

c2_status_t C2GoldfishAvcDec::onInit() {
    status_t err = initDecoder();
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2GoldfishAvcDec::onStop() {
    if (OK != resetDecoder())
        return C2_CORRUPTED;
    resetPlugin();
    return C2_OK;
}

void C2GoldfishAvcDec::onReset() { (void)onStop(); }

void C2GoldfishAvcDec::onRelease() {
    (void)deleteDecoder();
    if (mOutBlock) {
        mOutBlock.reset();
    }
}

c2_status_t C2GoldfishAvcDec::onFlush_sm() {
    if (OK != setFlushMode())
        return C2_CORRUPTED;

    if (!mContext) {
        // just ignore if context is not even created
        return C2_OK;
    }

    uint32_t bufferSize = mStride * mHeight * 3 / 2;
    mOutBufferFlush = (uint8_t *)ivd_aligned_malloc(nullptr, 128, bufferSize);
    if (!mOutBufferFlush) {
        ALOGE("could not allocate tmp output buffer (for flush) of size %u ",
              bufferSize);
        return C2_NO_MEMORY;
    }

    mContext->flush();
    while (true) {

        setDecodeArgs(nullptr, nullptr, 0, 0, 0);
        mImg = mContext->getImage();
        if (mImg.data == nullptr) {
            resetPlugin();
            break;
        }
    }

    if (mOutBufferFlush) {
        ivd_aligned_free(nullptr, mOutBufferFlush);
        mOutBufferFlush = nullptr;
    }

    return C2_OK;
}

status_t C2GoldfishAvcDec::createDecoder() {

    DDD("creating avc context now w %d h %d", mWidth, mHeight);
    if (mEnableAndroidNativeBuffers) {
        mContext.reset(new MediaH264Decoder(RenderMode::RENDER_BY_HOST_GPU));
    } else {
        mContext.reset(new MediaH264Decoder(RenderMode::RENDER_BY_GUEST_CPU));
    }
    mContext->initH264Context(mWidth, mHeight, mWidth, mHeight,
                              MediaH264Decoder::PixelFormat::YUV420P);
    return OK;
}

status_t C2GoldfishAvcDec::setParams(size_t stride) {
    (void)stride;
    return OK;
}

status_t C2GoldfishAvcDec::initDecoder() {
    //    if (OK != createDecoder()) return UNKNOWN_ERROR;
    mStride = ALIGN32(mWidth);
    mSignalledError = false;
    resetPlugin();

    return OK;
}

bool C2GoldfishAvcDec::setDecodeArgs(C2ReadView *inBuffer,
                                     C2GraphicView *outBuffer, size_t inOffset,
                                     size_t inSize, uint32_t tsMarker) {
    uint32_t displayStride = mStride;
    (void)inBuffer;
    (void)inOffset;
    (void)inSize;
    (void)tsMarker;
    if (outBuffer) {
        C2PlanarLayout layout;
        layout = outBuffer->layout();
        displayStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    }

    if (inBuffer) {
        //= tsMarker;
        mInPBuffer = const_cast<uint8_t *>(inBuffer->data() + inOffset);
        mInPBufferSize = inSize;
        mInTsMarker = tsMarker;
    }

    // uint32_t displayHeight = mHeight;
    // size_t lumaSize = displayStride * displayHeight;
    // size_t chromaSize = lumaSize >> 2;

    if (mStride != displayStride) {
        mStride = displayStride;
        if (OK != setParams(mStride))
            return false;
    }

    return true;
}

bool C2GoldfishAvcDec::getVuiParams() { return true; }

status_t C2GoldfishAvcDec::setFlushMode() {
    if (mContext) {
        mContext->flush();
    }
    return OK;
}

status_t C2GoldfishAvcDec::resetDecoder() {
    mStride = 0;
    mSignalledError = false;
    mHeaderDecoded = false;
    if (mContext) {
        // The resolution may have changed, so our safest bet is to just destroy
        // the current context and recreate another one, with the new width and
        // height.
        mContext->destroyH264Context();
        mContext.reset(nullptr);
    }

    return OK;
}

void C2GoldfishAvcDec::resetPlugin() {
    mSignalledOutputEos = false;
    gettimeofday(&mTimeStart, nullptr);
    gettimeofday(&mTimeEnd, nullptr);
}

status_t C2GoldfishAvcDec::deleteDecoder() {
    if (mContext) {
        mContext->destroyH264Context();
        mContext.reset(nullptr);
    }
    return OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        DDD("signalling eos");
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2GoldfishAvcDec::finishWork(uint64_t index,
                                  const std::unique_ptr<C2Work> &work) {
    std::shared_ptr<C2Buffer> buffer =
        createGraphicBuffer(std::move(mOutBlock), C2Rect(mWidth, mHeight));
    mOutBlock = nullptr;
    {
        IntfImpl::Lock lock = mIntf->lock();
        buffer->setInfo(mIntf->getColorAspects_l());
    }

    class FillWork {
      public:
        FillWork(uint32_t flags, C2WorkOrdinalStruct ordinal,
                 const std::shared_ptr<C2Buffer> &buffer)
            : mFlags(flags), mOrdinal(ordinal), mBuffer(buffer) {}
        ~FillWork() = default;

        void operator()(const std::unique_ptr<C2Work> &work) {
            work->worklets.front()->output.flags = (C2FrameData::flags_t)mFlags;
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.ordinal = mOrdinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
            if (mBuffer) {
                work->worklets.front()->output.buffers.push_back(mBuffer);
            }
            DDD("timestamp = %lld, index = %lld, w/%s buffer",
                mOrdinal.timestamp.peekll(), mOrdinal.frameIndex.peekll(),
                mBuffer ? "" : "o");
        }

      private:
        const uint32_t mFlags;
        const C2WorkOrdinalStruct mOrdinal;
        const std::shared_ptr<C2Buffer> mBuffer;
    };

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };
    if (work && c2_cntr64_t(index) == work->input.ordinal.frameIndex) {
        bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
        // TODO: Check if cloneAndSend can be avoided by tracking number of
        // frames remaining
        if (eos) {
            if (buffer) {
                mOutIndex = index;
                C2WorkOrdinalStruct outOrdinal = work->input.ordinal;
                DDD("%s %d: cloneAndSend ", __func__, __LINE__);
                cloneAndSend(
                    mOutIndex, work,
                    FillWork(C2FrameData::FLAG_INCOMPLETE, outOrdinal, buffer));
                buffer.reset();
            }
        } else {
            DDD("%s %d: fill", __func__, __LINE__);
            fillWork(work);
        }
    } else {
        DDD("%s %d: finish", __func__, __LINE__);
        finish(index, fillWork);
    }
}

c2_status_t
C2GoldfishAvcDec::ensureDecoderState(const std::shared_ptr<C2BlockPool> &pool) {
    if (mOutBlock && (mOutBlock->width() != ALIGN32(mWidth) ||
                      mOutBlock->height() != mHeight)) {
        mOutBlock.reset();
    }
    if (!mOutBlock) {
        // uint32_t format = HAL_PIXEL_FORMAT_YCBCR_420_888;//19;
        // //HAL_PIXEL_FORMAT_YV12;
        uint32_t format = 19; // HAL_PIXEL_FORMAT_YV12;
        C2MemoryUsage usage = {C2MemoryUsage::CPU_READ,
                               C2MemoryUsage::CPU_WRITE};
        usage.expected = (uint64_t)(BufferUsage::GPU_DATA_BUFFER);
        // C2MemoryUsage usage = {(unsigned
        // int)(BufferUsage::GPU_DATA_BUFFER)};// { C2MemoryUsage::CPU_READ,
        // C2MemoryUsage::CPU_WRITE };
        c2_status_t err = pool->fetchGraphicBlock(ALIGN32(mWidth), mHeight,
                                                  format, usage, &mOutBlock);
        if (err != C2_OK) {
            ALOGE("fetchGraphicBlock for Output failed with status %d", err);
            return err;
        }
        if (mEnableAndroidNativeBuffers) {
            auto c2Handle = mOutBlock->handle();
            native_handle_t *grallocHandle =
                UnwrapNativeCodec2GrallocHandle(c2Handle);
            cb_handle_t *cbhandle = (cb_handle_t *)grallocHandle;
            DDD("found handle %d", cbhandle->hostHandle);
            mHostColorBufferId = cbhandle->hostHandle;
        } else {
            C2GraphicView wView = mOutBlock->map().get();
            if (wView.error()) {
                ALOGE("graphic view map failed %d", wView.error());
                return C2_CORRUPTED;
            }
            mByteBuffer =
                const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_Y]);
        }
        DDD("provided (%dx%d) required (%dx%d)", mOutBlock->width(),
            mOutBlock->height(), ALIGN32(mWidth), mHeight);
    }

    return C2_OK;
}

void C2GoldfishAvcDec::checkMode(const std::shared_ptr<C2BlockPool> &pool) {
    mWidth = mIntf->width();
    mHeight = mIntf->height();
    {
        // now get the block
        constexpr uint32_t format = 19;
        std::shared_ptr<C2GraphicBlock> block;
        C2MemoryUsage usage = {C2MemoryUsage::CPU_READ,
                               C2MemoryUsage::CPU_WRITE};
        usage.expected = (uint64_t)(BufferUsage::GPU_DATA_BUFFER);

        c2_status_t err = pool->fetchGraphicBlock(align(mWidth, 16), mHeight,
                                                  format, usage, &block);
        if (err != C2_OK) {
            ALOGE("fetchGraphicBlock for Output failed with status %d", err);
            return;
        }
        auto c2Handle = block->handle();
        native_handle_t *grallocHandle =
            UnwrapNativeCodec2GrallocHandle(c2Handle);
        cb_handle_t *cbhandle = (cb_handle_t *)grallocHandle;
        int hostColorBufferId = cbhandle->hostHandle;
        if (hostColorBufferId > 0) {
            DDD("decoding to host color buffer");
            mEnableAndroidNativeBuffers = true;
        } else {
            DDD("decoding to guest byte buffer");
            mEnableAndroidNativeBuffers = false;
        }
    }
}

void C2GoldfishAvcDec::copyImageData(uint8_t *pBuffer, h264_image_t &img) {
    if (mEnableAndroidNativeBuffers)
        return;
    int myStride = mWidth;
    for (int i = 0; i < mHeight; ++i) {
        memcpy(pBuffer + i * myStride, img.data + i * mWidth, mWidth);
    }
    int Y = myStride * mHeight;
    for (int i = 0; i < mHeight / 2; ++i) {
        memcpy(pBuffer + Y + i * myStride / 2,
               img.data + mWidth * mHeight + i * mWidth / 2, mWidth / 2);
    }
    int UV = Y / 4;
    for (int i = 0; i < mHeight / 2; ++i) {
        memcpy(pBuffer + Y + UV + i * myStride / 2,
               img.data + mWidth * mHeight * 5 / 4 + i * mWidth / 2,
               mWidth / 2);
    }
}

// TODO: can overall error checking be improved?
// TODO: allow configuration of color format and usage for graphic buffers
// instead
//       of hard coding them to HAL_PIXEL_FORMAT_YV12
// TODO: pass coloraspects information to surface
// TODO: test support for dynamic change in resolution
// TODO: verify if the decoder sent back all frames
void C2GoldfishAvcDec::process(const std::unique_ptr<C2Work> &work,
                               const std::shared_ptr<C2BlockPool> &pool) {
    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    DDD("process work");
    if (!mContext) {
        DDD("creating decoder context to host in process work");
        checkMode(pool);
        createDecoder();
    }

    size_t inOffset = 0u;
    size_t inSize = 0u;
    uint32_t workIndex = work->input.ordinal.frameIndex.peeku() & 0xFFFFFFFF;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView =
            work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            ALOGE("read view map failed %d", rView.error());
            work->result = rView.error();
            return;
        }
    }
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    bool hasPicture = false;

    DDD("in buffer attr. size %zu timestamp %d frameindex %d, flags %x", inSize,
        (int)work->input.ordinal.timestamp.peeku(),
        (int)work->input.ordinal.frameIndex.peeku(), work->input.flags);
    size_t inPos = 0;
    while (inPos < inSize && inSize - inPos >= kMinInputBytes) {
        if (C2_OK != ensureDecoderState(pool)) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }

        {
            // C2GraphicView wView;// = mOutBlock->map().get();
            // if (wView.error()) {
            //    ALOGE("graphic view map failed %d", wView.error());
            //    work->result = wView.error();
            //    return;
            //}
            if (!setDecodeArgs(&rView, nullptr, inOffset + inPos,
                               inSize - inPos, workIndex)) {
                mSignalledError = true;
                work->workletsProcessed = 1u;
                work->result = C2_CORRUPTED;
                return;
            }

            if (false == mHeaderDecoded) {
                /* Decode header and get dimensions */
                setParams(mStride);
            }

            uint32_t delay;
            GETTIME(&mTimeStart, nullptr);
            TIME_DIFF(mTimeEnd, mTimeStart, delay);
            //(void) ivdec_api_function(mDecHandle, &s_decode_ip, &s_decode_op);
            DDD("decoding");
            h264_result_t h264Res =
                mContext->decodeFrame(mInPBuffer, mInPBufferSize, mInTsMarker);
            mConsumedBytes = h264Res.bytesProcessed;

            if (mHostColorBufferId > 0) {
                mImg = mContext->renderOnHostAndReturnImageMetadata(
                    mHostColorBufferId);
            } else {
                mImg = mContext->getImage();
            }
            uint32_t decodeTime;
            GETTIME(&mTimeEnd, nullptr);
            TIME_DIFF(mTimeStart, mTimeEnd, decodeTime);
        }
        // TODO: handle res change
        if (0) {
            DDD("resolution changed");
            drainInternal(DRAIN_COMPONENT_NO_EOS, pool, work);
            resetDecoder();
            resetPlugin();
            work->workletsProcessed = 0u;

            /* Decode header and get new dimensions */
            setParams(mStride);
            //            (void) ivdec_api_function(mDecHandle, &s_decode_ip,
            //            &s_decode_op);
        }
        (void)getVuiParams();
        if (mImg.data != nullptr) {
            DDD("got data %" PRIu64, mImg.pts);
            hasPicture = true;
            copyImageData(mByteBuffer, mImg);
            finishWork(mImg.pts, work);
        } else {
            work->workletsProcessed = 0u;
        }

        inPos += mConsumedBytes;
    }
    if (eos) {
        DDD("drain because of eos");
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledOutputEos = true;
    } else if (!hasPicture) {
        fillEmptyWork(work);
        work->workletsProcessed = 0u;
    }

    work->input.buffers.clear();
}

c2_status_t
C2GoldfishAvcDec::drainInternal(uint32_t drainMode,
                                const std::shared_ptr<C2BlockPool> &pool,
                                const std::unique_ptr<C2Work> &work) {
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    if (OK != setFlushMode())
        return C2_CORRUPTED;
    while (true) {
        if (C2_OK != ensureDecoderState(pool)) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return C2_CORRUPTED;
        }
        /*
        C2GraphicView wView = mOutBlock->map().get();
        if (wView.error()) {
            ALOGE("graphic view map failed %d", wView.error());
            return C2_CORRUPTED;
        }
        if (!setDecodeArgs(nullptr, &wView, 0, 0, 0)) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            return C2_CORRUPTED;
        }
        */

        if (mHostColorBufferId > 0) {
            mImg = mContext->renderOnHostAndReturnImageMetadata(
                mHostColorBufferId);
        } else {
            mImg = mContext->getImage();
        }

        // TODO: maybe keep rendering to screen
        //        mImg = mContext->getImage();
        if (mImg.data != nullptr) {
            DDD("got data in drain mode %" PRIu64, mImg.pts);
            copyImageData(mByteBuffer, mImg);
            finishWork(mImg.pts, work);
        } else {
            fillEmptyWork(work);
            break;
        }
    }

    return C2_OK;
}

c2_status_t C2GoldfishAvcDec::drain(uint32_t drainMode,
                                    const std::shared_ptr<C2BlockPool> &pool) {
    DDD("drainInternal because of drain");
    return drainInternal(drainMode, pool, nullptr);
}

class C2GoldfishAvcDecFactory : public C2ComponentFactory {
  public:
    C2GoldfishAvcDecFactory()
        : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
              GoldfishComponentStore::Create()->getParamReflector())) {}

    virtual c2_status_t
    createComponent(c2_node_id_t id,
                    std::shared_ptr<C2Component> *const component,
                    std::function<void(C2Component *)> deleter) override {
        *component = std::shared_ptr<C2Component>(
            new C2GoldfishAvcDec(
                COMPONENT_NAME, id,
                std::make_shared<C2GoldfishAvcDec::IntfImpl>(mHelper)),
            deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
        c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *const interface,
        std::function<void(C2ComponentInterface *)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
            new SimpleInterface<C2GoldfishAvcDec::IntfImpl>(
                COMPONENT_NAME, id,
                std::make_shared<C2GoldfishAvcDec::IntfImpl>(mHelper)),
            deleter);
        return C2_OK;
    }

    virtual ~C2GoldfishAvcDecFactory() override = default;

  private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

} // namespace android

extern "C" ::C2ComponentFactory *CreateCodec2Factory() {
    DDD("in %s", __func__);
    return new ::android::C2GoldfishAvcDecFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory *factory) {
    DDD("in %s", __func__);
    delete factory;
}