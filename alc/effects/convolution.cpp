
#include "config.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <stdint.h>
#include <utility>

#ifdef HAVE_SSE_INTRINSICS
#include <xmmintrin.h>
#elif defined(HAVE_NEON)
#include <arm_neon.h>
#endif

#include "alcomplex.h"
#include "almalloc.h"
#include "alnumbers.h"
#include "alnumeric.h"
#include "alspan.h"
#include "base.h"
#include "core/ambidefs.h"
#include "core/bufferline.h"
#include "core/buffer_storage.h"
#include "core/context.h"
#include "core/devformat.h"
#include "core/device.h"
#include "core/effectslot.h"
#include "core/filters/splitter.h"
#include "core/fmt_traits.h"
#include "core/mixer.h"
#include "intrusive_ptr.h"
#include "pffft.h"
#include "polyphase_resampler.h"
#include "vector.h"


namespace {

/* Convolution reverb is implemented using a segmented overlap-add method. The
 * impulse response is broken up into multiple segments of 128 samples, and
 * each segment has an FFT applied with a 256-sample buffer (the latter half
 * left silent) to get its frequency-domain response. The resulting response
 * has its positive/non-mirrored frequencies saved (129 bins) in each segment.
 * Note that since the 0th and 129th bins are real for a real signal, their
 * imaginary components are always 0 and can be dropped, allowing their real
 * components to be combined so only 128 complex values are stored for the 129
 * bins.
 *
 * Input samples are similarly broken up into 128-sample segments, with an FFT
 * applied to each new incoming segment to get its 129 bins. A history of FFT'd
 * input segments is maintained, equal to the length of the impulse response.
 *
 * To apply the reverberation, each impulse response segment is convolved with
 * its paired input segment (using complex multiplies, far cheaper than FIRs),
 * accumulating into a 129-bin FFT buffer. The input history is then shifted to
 * align with later impulse response segments for the next input segment.
 *
 * An inverse FFT is then applied to the accumulated FFT buffer to get a 256-
 * sample time-domain response for output, which is split in two halves. The
 * first half is the 128-sample output, and the second half is a 128-sample
 * (really, 127) delayed extension, which gets added to the output next time.
 * Convolving two time-domain responses of lengths N and M results in a time-
 * domain signal of length N+M-1, and this holds true regardless of the
 * convolution being applied in the frequency domain, so these "overflow"
 * samples need to be accounted for.
 *
 * To avoid a delay with gathering enough input samples to apply an FFT with,
 * the first segment is applied directly in the time-domain as the samples come
 * in. Once enough have been retrieved, the FFT is applied on the input and
 * it's paired with the remaining (FFT'd) filter segments for processing.
 */


void LoadSamples(float *RESTRICT dst, const std::byte *src, const size_t srcstep, FmtType srctype,
    const size_t samples) noexcept
{
#define HANDLE_FMT(T)  case T: al::LoadSampleArray<T>(dst, src, srcstep, samples); break
    switch(srctype)
    {
    HANDLE_FMT(FmtUByte);
    HANDLE_FMT(FmtShort);
    HANDLE_FMT(FmtFloat);
    HANDLE_FMT(FmtDouble);
    HANDLE_FMT(FmtMulaw);
    HANDLE_FMT(FmtAlaw);
    /* FIXME: Handle ADPCM decoding here. */
    case FmtIMA4:
    case FmtMSADPCM:
        std::fill_n(dst, samples, 0.0f);
        break;
    }
#undef HANDLE_FMT
}


constexpr auto GetAmbiScales(AmbiScaling scaletype) noexcept
{
    switch(scaletype)
    {
    case AmbiScaling::FuMa: return al::span{AmbiScale::FromFuMa};
    case AmbiScaling::SN3D: return al::span{AmbiScale::FromSN3D};
    case AmbiScaling::UHJ: return al::span{AmbiScale::FromUHJ};
    case AmbiScaling::N3D: break;
    }
    return al::span{AmbiScale::FromN3D};
}

constexpr auto GetAmbiLayout(AmbiLayout layouttype) noexcept
{
    if(layouttype == AmbiLayout::FuMa) return al::span{AmbiIndex::FromFuMa};
    return al::span{AmbiIndex::FromACN};
}

constexpr auto GetAmbi2DLayout(AmbiLayout layouttype) noexcept
{
    if(layouttype == AmbiLayout::FuMa) return al::span{AmbiIndex::FromFuMa2D};
    return al::span{AmbiIndex::FromACN2D};
}


constexpr float sin30{0.5f};
constexpr float cos30{0.866025403785f};
constexpr float sin45{al::numbers::sqrt2_v<float>*0.5f};
constexpr float cos45{al::numbers::sqrt2_v<float>*0.5f};
constexpr float sin110{ 0.939692620786f};
constexpr float cos110{-0.342020143326f};

struct ChanPosMap {
    Channel channel;
    std::array<float,3> pos;
};


using complex_f = std::complex<float>;

constexpr size_t ConvolveUpdateSize{256};
constexpr size_t ConvolveUpdateSamples{ConvolveUpdateSize / 2};


void apply_fir(al::span<float> dst, const float *RESTRICT src, const float *RESTRICT filter)
{
#ifdef HAVE_SSE_INTRINSICS
    for(float &output : dst)
    {
        __m128 r4{_mm_setzero_ps()};
        for(size_t j{0};j < ConvolveUpdateSamples;j+=4)
        {
            const __m128 coeffs{_mm_load_ps(&filter[j])};
            const __m128 s{_mm_loadu_ps(&src[j])};

            r4 = _mm_add_ps(r4, _mm_mul_ps(s, coeffs));
        }
        r4 = _mm_add_ps(r4, _mm_shuffle_ps(r4, r4, _MM_SHUFFLE(0, 1, 2, 3)));
        r4 = _mm_add_ps(r4, _mm_movehl_ps(r4, r4));
        output = _mm_cvtss_f32(r4);

        ++src;
    }

#elif defined(HAVE_NEON)

    for(float &output : dst)
    {
        float32x4_t r4{vdupq_n_f32(0.0f)};
        for(size_t j{0};j < ConvolveUpdateSamples;j+=4)
            r4 = vmlaq_f32(r4, vld1q_f32(&src[j]), vld1q_f32(&filter[j]));
        r4 = vaddq_f32(r4, vrev64q_f32(r4));
        output = vget_lane_f32(vadd_f32(vget_low_f32(r4), vget_high_f32(r4)), 0);

        ++src;
    }

#else

    for(float &output : dst)
    {
        float ret{0.0f};
        for(size_t j{0};j < ConvolveUpdateSamples;++j)
            ret += src[j] * filter[j];
        output = ret;
        ++src;
    }
#endif
}


template<typename T>
struct AlignedDeleter { };

template<typename T>
struct AlignedDeleter<T[]> {
    static_assert(std::is_trivially_destructible_v<T>);
    using type = T;

    void operator()(T *ptr) { al_free(ptr); }
};
template<typename T>
using AlignedUPtr = std::unique_ptr<T,AlignedDeleter<T>>;

template<typename T, size_t A>
auto MakeAlignedPtr(size_t count) -> AlignedUPtr<T>
{
    using Type = typename AlignedDeleter<T>::type;
    void *ptr{al_calloc(A, sizeof(Type)*count)};
    return AlignedUPtr<T>{::new(ptr) Type[count]};
}


struct PFFFTSetupDeleter {
    void operator()(PFFFT_Setup *ptr) { pffft_destroy_setup(ptr); }
};
using PFFFTSetupPtr = std::unique_ptr<PFFFT_Setup,PFFFTSetupDeleter>;


struct ConvolutionState final : public EffectState {
    FmtChannels mChannels{};
    AmbiLayout mAmbiLayout{};
    AmbiScaling mAmbiScaling{};
    uint mAmbiOrder{};

    size_t mFifoPos{0};
    std::array<float,ConvolveUpdateSamples*2> mInput{};
    al::vector<std::array<float,ConvolveUpdateSamples>,16> mFilter;
    al::vector<std::array<float,ConvolveUpdateSamples*2>,16> mOutput;

    PFFFTSetupPtr mFft{};
    alignas(16) std::array<float,ConvolveUpdateSize> mFftBuffer{};
    alignas(16) std::array<float,ConvolveUpdateSize> mFftWorkBuffer{};

    size_t mCurrentSegment{0};
    size_t mNumConvolveSegs{0};

    struct ChannelData {
        alignas(16) FloatBufferLine mBuffer{};
        float mHfScale{}, mLfScale{};
        BandSplitter mFilter{};
        float Current[MAX_OUTPUT_CHANNELS]{};
        float Target[MAX_OUTPUT_CHANNELS]{};
    };
    using ChannelDataArray = al::FlexArray<ChannelData>;
    std::unique_ptr<ChannelDataArray> mChans;
    AlignedUPtr<float[]> mComplexData;


    ConvolutionState() = default;
    ~ConvolutionState() override = default;

    void NormalMix(const al::span<FloatBufferLine> samplesOut, const size_t samplesToDo);
    void UpsampleMix(const al::span<FloatBufferLine> samplesOut, const size_t samplesToDo);
    void (ConvolutionState::*mMix)(const al::span<FloatBufferLine>,const size_t)
    {&ConvolutionState::NormalMix};

    void deviceUpdate(const DeviceBase *device, const BufferStorage *buffer) override;
    void update(const ContextBase *context, const EffectSlot *slot, const EffectProps *props,
        const EffectTarget target) override;
    void process(const size_t samplesToDo, const al::span<const FloatBufferLine> samplesIn,
        const al::span<FloatBufferLine> samplesOut) override;

    DEF_NEWDEL(ConvolutionState)
};

void ConvolutionState::NormalMix(const al::span<FloatBufferLine> samplesOut,
    const size_t samplesToDo)
{
    for(auto &chan : *mChans)
        MixSamples({chan.mBuffer.data(), samplesToDo}, samplesOut, chan.Current, chan.Target,
            samplesToDo, 0);
}

void ConvolutionState::UpsampleMix(const al::span<FloatBufferLine> samplesOut,
    const size_t samplesToDo)
{
    for(auto &chan : *mChans)
    {
        const al::span<float> src{chan.mBuffer.data(), samplesToDo};
        chan.mFilter.processScale(src, chan.mHfScale, chan.mLfScale);
        MixSamples(src, samplesOut, chan.Current, chan.Target, samplesToDo, 0);
    }
}


void ConvolutionState::deviceUpdate(const DeviceBase *device, const BufferStorage *buffer)
{
    using UhjDecoderType = UhjDecoder<512>;
    static constexpr auto DecoderPadding = UhjDecoderType::sInputPadding;

    static constexpr uint MaxConvolveAmbiOrder{1u};

    if(!mFft)
        mFft = PFFFTSetupPtr{pffft_new_setup(ConvolveUpdateSize, PFFFT_REAL)};

    mFifoPos = 0;
    mInput.fill(0.0f);
    decltype(mFilter){}.swap(mFilter);
    decltype(mOutput){}.swap(mOutput);
    mFftBuffer.fill(0.0f);
    mFftWorkBuffer.fill(0.0f);

    mCurrentSegment = 0;
    mNumConvolveSegs = 0;

    mChans = nullptr;
    mComplexData = nullptr;

    /* An empty buffer doesn't need a convolution filter. */
    if(!buffer || buffer->mSampleLen < 1) return;

    mChannels = buffer->mChannels;
    mAmbiLayout = IsUHJ(mChannels) ? AmbiLayout::FuMa : buffer->mAmbiLayout;
    mAmbiScaling = IsUHJ(mChannels) ? AmbiScaling::UHJ : buffer->mAmbiScaling;
    mAmbiOrder = minu(buffer->mAmbiOrder, MaxConvolveAmbiOrder);

    const auto bytesPerSample = BytesFromFmt(buffer->mType);
    const auto realChannels = buffer->channelsFromFmt();
    const auto numChannels = (mChannels == FmtUHJ2) ? 3u : ChannelsFromFmt(mChannels, mAmbiOrder);

    mChans = ChannelDataArray::Create(numChannels);

    /* The impulse response needs to have the same sample rate as the input and
     * output. The bsinc24 resampler is decent, but there is high-frequency
     * attenuation that some people may be able to pick up on. Since this is
     * called very infrequently, go ahead and use the polyphase resampler.
     */
    PPhaseResampler resampler;
    if(device->Frequency != buffer->mSampleRate)
        resampler.init(buffer->mSampleRate, device->Frequency);
    const auto resampledCount = static_cast<uint>(
        (uint64_t{buffer->mSampleLen}*device->Frequency+(buffer->mSampleRate-1)) /
        buffer->mSampleRate);

    const BandSplitter splitter{device->mXOverFreq / static_cast<float>(device->Frequency)};
    for(auto &e : *mChans)
        e.mFilter = splitter;

    mFilter.resize(numChannels, {});
    mOutput.resize(numChannels, {});

    /* Calculate the number of segments needed to hold the impulse response and
     * the input history (rounded up), and allocate them. Exclude one segment
     * which gets applied as a time-domain FIR filter. Make sure at least one
     * segment is allocated to simplify handling.
     */
    mNumConvolveSegs = (resampledCount+(ConvolveUpdateSamples-1)) / ConvolveUpdateSamples;
    mNumConvolveSegs = maxz(mNumConvolveSegs, 2) - 1;

    const size_t complex_length{mNumConvolveSegs * ConvolveUpdateSize * (numChannels+1)};
    mComplexData = MakeAlignedPtr<float[],16>(complex_length);
    std::fill_n(mComplexData.get(), complex_length, 0.0f);

    /* Load the samples from the buffer. */
    const size_t srclinelength{RoundUp(buffer->mSampleLen+DecoderPadding, 16)};
    auto srcsamples = std::make_unique<float[]>(srclinelength * numChannels);
    std::fill_n(srcsamples.get(), srclinelength * numChannels, 0.0f);
    for(size_t c{0};c < numChannels && c < realChannels;++c)
        LoadSamples(srcsamples.get() + srclinelength*c, buffer->mData.data() + bytesPerSample*c,
            realChannels, buffer->mType, buffer->mSampleLen);

    if(IsUHJ(mChannels))
    {
        auto decoder = std::make_unique<UhjDecoderType>();
        std::array<float*,4> samples{};
        for(size_t c{0};c < numChannels;++c)
            samples[c] = srcsamples.get() + srclinelength*c;
        decoder->decode({samples.data(), numChannels}, buffer->mSampleLen, buffer->mSampleLen);
    }

    auto ressamples = std::make_unique<double[]>(buffer->mSampleLen +
        (resampler ? resampledCount : 0));
    auto ffttmp = al::vector<float,16>(ConvolveUpdateSize);
    auto fftbuffer = std::vector<std::complex<double>>(ConvolveUpdateSize);

    float *filteriter = mComplexData.get() + mNumConvolveSegs*ConvolveUpdateSize;
    for(size_t c{0};c < numChannels;++c)
    {
        /* Resample to match the device. */
        if(resampler)
        {
            std::copy_n(srcsamples.get() + srclinelength*c, buffer->mSampleLen,
                ressamples.get() + resampledCount);
            resampler.process(buffer->mSampleLen, ressamples.get()+resampledCount,
                resampledCount, ressamples.get());
        }
        else
            std::copy_n(srcsamples.get() + srclinelength*c, buffer->mSampleLen, ressamples.get());

        /* Store the first segment's samples in reverse in the time-domain, to
         * apply as a FIR filter.
         */
        const size_t first_size{minz(resampledCount, ConvolveUpdateSamples)};
        std::transform(ressamples.get(), ressamples.get()+first_size, mFilter[c].rbegin(),
            [](const double d) noexcept -> float { return static_cast<float>(d); });

        size_t done{first_size};
        for(size_t s{0};s < mNumConvolveSegs;++s)
        {
            const size_t todo{minz(resampledCount-done, ConvolveUpdateSamples)};

            /* Apply a double-precision forward FFT for more precise frequency
             * measurements.
             */
            auto iter = std::copy_n(&ressamples[done], todo, fftbuffer.begin());
            done += todo;
            std::fill(iter, fftbuffer.end(), std::complex<double>{});
            forward_fft(al::span{fftbuffer});

            /* Convert to, and pack in, a float buffer for PFFFT. Note that the
             * first bin stores the real component of the half-frequency bin in
             * the imaginary component.
             */
            for(size_t i{0};i < ConvolveUpdateSamples;++i)
            {
                ffttmp[i*2    ] = static_cast<float>(fftbuffer[i].real());
                ffttmp[i*2 + 1] = static_cast<float>((i == 0) ?
                    fftbuffer[ConvolveUpdateSamples+1].real() : fftbuffer[i].imag());
            }
            /* Reorder backward to make it suitable for pffft_zconvolve and the
             * subsequent pffft_transform(..., PFFFT_BACKWARD).
             */
            pffft_zreorder(mFft.get(), ffttmp.data(), al::to_address(filteriter), PFFFT_BACKWARD);
            filteriter += ConvolveUpdateSize;
        }
    }
}


void ConvolutionState::update(const ContextBase *context, const EffectSlot *slot,
    const EffectProps* /*props*/, const EffectTarget target)
{
    /* NOTE: Stereo and Rear are slightly different from normal mixing (as
     * defined in alu.cpp). These are 45 degrees from center, rather than the
     * 30 degrees used there.
     *
     * TODO: LFE is not mixed to output. This will require each buffer channel
     * to have its own output target since the main mixing buffer won't have an
     * LFE channel (due to being B-Format).
     */
    static constexpr ChanPosMap MonoMap[1]{
        { FrontCenter, std::array{0.0f, 0.0f, -1.0f} }
    }, StereoMap[2]{
        { FrontLeft,  std::array{-sin45, 0.0f, -cos45} },
        { FrontRight, std::array{ sin45, 0.0f, -cos45} },
    }, RearMap[2]{
        { BackLeft,  std::array{-sin45, 0.0f, cos45} },
        { BackRight, std::array{ sin45, 0.0f, cos45} },
    }, QuadMap[4]{
        { FrontLeft,  std::array{-sin45, 0.0f, -cos45} },
        { FrontRight, std::array{ sin45, 0.0f, -cos45} },
        { BackLeft,   std::array{-sin45, 0.0f,  cos45} },
        { BackRight,  std::array{ sin45, 0.0f,  cos45} },
    }, X51Map[6]{
        { FrontLeft,   std::array{-sin30, 0.0f, -cos30} },
        { FrontRight,  std::array{ sin30, 0.0f, -cos30} },
        { FrontCenter, std::array{  0.0f, 0.0f, -1.0f} },
        { LFE, {} },
        { SideLeft,    std::array{-sin110, 0.0f, -cos110} },
        { SideRight,   std::array{ sin110, 0.0f, -cos110} },
    }, X61Map[7]{
        { FrontLeft,   std::array{-sin30, 0.0f, -cos30} },
        { FrontRight,  std::array{ sin30, 0.0f, -cos30} },
        { FrontCenter, std::array{  0.0f, 0.0f, -1.0f} },
        { LFE, {} },
        { BackCenter,  std::array{ 0.0f, 0.0f, 1.0f} },
        { SideLeft,    std::array{-1.0f, 0.0f, 0.0f} },
        { SideRight,   std::array{ 1.0f, 0.0f, 0.0f} },
    }, X71Map[8]{
        { FrontLeft,   std::array{-sin30, 0.0f, -cos30} },
        { FrontRight,  std::array{ sin30, 0.0f, -cos30} },
        { FrontCenter, std::array{  0.0f, 0.0f, -1.0f} },
        { LFE, {} },
        { BackLeft,    std::array{-sin30, 0.0f, cos30} },
        { BackRight,   std::array{ sin30, 0.0f, cos30} },
        { SideLeft,    std::array{ -1.0f, 0.0f, 0.0f} },
        { SideRight,   std::array{  1.0f, 0.0f, 0.0f} },
    };

    if(mNumConvolveSegs < 1) UNLIKELY
        return;

    mMix = &ConvolutionState::NormalMix;

    for(auto &chan : *mChans)
        std::fill(std::begin(chan.Target), std::end(chan.Target), 0.0f);
    const float gain{slot->Gain};
    if(IsAmbisonic(mChannels))
    {
        DeviceBase *device{context->mDevice};
        if(mChannels == FmtUHJ2 && !device->mUhjEncoder)
        {
            mMix = &ConvolutionState::UpsampleMix;
            (*mChans)[0].mHfScale = 1.0f;
            (*mChans)[0].mLfScale = DecoderBase::sWLFScale;
            (*mChans)[1].mHfScale = 1.0f;
            (*mChans)[1].mLfScale = DecoderBase::sXYLFScale;
            (*mChans)[2].mHfScale = 1.0f;
            (*mChans)[2].mLfScale = DecoderBase::sXYLFScale;
        }
        else if(device->mAmbiOrder > mAmbiOrder)
        {
            mMix = &ConvolutionState::UpsampleMix;
            const auto scales = AmbiScale::GetHFOrderScales(mAmbiOrder, device->mAmbiOrder,
                device->m2DMixing);
            (*mChans)[0].mHfScale = scales[0];
            (*mChans)[0].mLfScale = 1.0f;
            for(size_t i{1};i < mChans->size();++i)
            {
                (*mChans)[i].mHfScale = scales[1];
                (*mChans)[i].mLfScale = 1.0f;
            }
        }
        mOutTarget = target.Main->Buffer;

        const auto scales = GetAmbiScales(mAmbiScaling);
        const uint8_t *index_map{Is2DAmbisonic(mChannels) ?
            GetAmbi2DLayout(mAmbiLayout).data() :
            GetAmbiLayout(mAmbiLayout).data()};

        std::array<float,MaxAmbiChannels> coeffs{};
        for(size_t c{0u};c < mChans->size();++c)
        {
            const size_t acn{index_map[c]};
            coeffs[acn] = scales[acn];
            ComputePanGains(target.Main, coeffs.data(), gain, (*mChans)[c].Target);
            coeffs[acn] = 0.0f;
        }
    }
    else
    {
        DeviceBase *device{context->mDevice};
        al::span<const ChanPosMap> chanmap{};
        switch(mChannels)
        {
        case FmtMono: chanmap = MonoMap; break;
        case FmtSuperStereo:
        case FmtStereo: chanmap = StereoMap; break;
        case FmtRear: chanmap = RearMap; break;
        case FmtQuad: chanmap = QuadMap; break;
        case FmtX51: chanmap = X51Map; break;
        case FmtX61: chanmap = X61Map; break;
        case FmtX71: chanmap = X71Map; break;
        case FmtBFormat2D:
        case FmtBFormat3D:
        case FmtUHJ2:
        case FmtUHJ3:
        case FmtUHJ4:
            break;
        }

        mOutTarget = target.Main->Buffer;
        if(device->mRenderMode == RenderMode::Pairwise)
        {
            /* Scales the azimuth of the given vector by 3 if it's in front.
             * Effectively scales +/-45 degrees to +/-90 degrees, leaving > +90
             * and < -90 alone.
             */
            auto ScaleAzimuthFront = [](std::array<float,3> pos) -> std::array<float,3>
            {
                if(pos[2] < 0.0f)
                {
                    /* Normalize the length of the x,z components for a 2D
                     * vector of the azimuth angle. Negate Z since {0,0,-1} is
                     * angle 0.
                     */
                    const float len2d{std::sqrt(pos[0]*pos[0] + pos[2]*pos[2])};
                    float x{pos[0] / len2d};
                    float z{-pos[2] / len2d};

                    /* Z > cos(pi/4) = -45 < azimuth < 45 degrees. */
                    if(z > cos45)
                    {
                        /* Double the angle represented by x,z. */
                        const float resx{2.0f*x * z};
                        const float resz{z*z - x*x};

                        /* Scale the vector back to fit in 3D. */
                        pos[0] = resx * len2d;
                        pos[2] = -resz * len2d;
                    }
                    else
                    {
                        /* If azimuth >= 45 degrees, clamp to 90 degrees. */
                        pos[0] = std::copysign(len2d, pos[0]);
                        pos[2] = 0.0f;
                    }
                }
                return pos;
            };

            for(size_t i{0};i < chanmap.size();++i)
            {
                if(chanmap[i].channel == LFE) continue;
                const auto coeffs = CalcDirectionCoeffs(ScaleAzimuthFront(chanmap[i].pos), 0.0f);
                ComputePanGains(target.Main, coeffs.data(), gain, (*mChans)[i].Target);
            }
        }
        else for(size_t i{0};i < chanmap.size();++i)
        {
            if(chanmap[i].channel == LFE) continue;
            const auto coeffs = CalcDirectionCoeffs(chanmap[i].pos, 0.0f);
            ComputePanGains(target.Main, coeffs.data(), gain, (*mChans)[i].Target);
        }
    }
}

void ConvolutionState::process(const size_t samplesToDo,
    const al::span<const FloatBufferLine> samplesIn, const al::span<FloatBufferLine> samplesOut)
{
    if(mNumConvolveSegs < 1) UNLIKELY
        return;

    size_t curseg{mCurrentSegment};
    auto &chans = *mChans;

    for(size_t base{0u};base < samplesToDo;)
    {
        const size_t todo{minz(ConvolveUpdateSamples-mFifoPos, samplesToDo-base)};

        std::copy_n(samplesIn[0].begin() + base, todo,
            mInput.begin()+ConvolveUpdateSamples+mFifoPos);

        /* Apply the FIR for the newly retrieved input samples, and combine it
         * with the inverse FFT'd output samples.
         */
        for(size_t c{0};c < chans.size();++c)
        {
            auto buf_iter = chans[c].mBuffer.begin() + base;
            apply_fir({buf_iter, todo}, mInput.data()+1 + mFifoPos, mFilter[c].data());

            auto fifo_iter = mOutput[c].begin() + mFifoPos;
            std::transform(fifo_iter, fifo_iter+todo, buf_iter, buf_iter, std::plus<>{});
        }

        mFifoPos += todo;
        base += todo;

        /* Check whether the input buffer is filled with new samples. */
        if(mFifoPos < ConvolveUpdateSamples) break;
        mFifoPos = 0;

        /* Move the newest input to the front for the next iteration's history. */
        std::copy(mInput.cbegin()+ConvolveUpdateSamples, mInput.cend(), mInput.begin());

        /* Calculate the frequency domain response and add the relevant
         * frequency bins to the FFT history.
         */
        auto fftiter = std::copy_n(mInput.cbegin(), ConvolveUpdateSamples, mFftBuffer.begin());
        std::fill(fftiter, mFftBuffer.end(), 0.0f);
        pffft_transform(mFft.get(), mFftBuffer.data(),
            mComplexData.get() + curseg*ConvolveUpdateSize, mFftWorkBuffer.data(), PFFFT_FORWARD);

        const float *RESTRICT filter{mComplexData.get() + mNumConvolveSegs*ConvolveUpdateSize};
        for(size_t c{0};c < chans.size();++c)
        {
            /* The iFFT'd response is scaled up by the number of bins, so apply
             * the inverse to normalize the output.
             */
            static constexpr float fftscale{1.0f / float{ConvolveUpdateSize}};

            /* Convolve each input segment with its IR filter counterpart
             * (aligned in time).
             */
            mFftBuffer.fill(0.0f);
            const float *RESTRICT input{&mComplexData[curseg*ConvolveUpdateSize]};
            for(size_t s{curseg};s < mNumConvolveSegs;++s)
            {
                pffft_zconvolve_accumulate(mFft.get(), input, filter, mFftBuffer.data(), fftscale);
                input += ConvolveUpdateSize;
                filter += ConvolveUpdateSize;
            }
            input = mComplexData.get();
            for(size_t s{0};s < curseg;++s)
            {
                pffft_zconvolve_accumulate(mFft.get(), input, filter, mFftBuffer.data(), fftscale);
                input += ConvolveUpdateSize;
                filter += ConvolveUpdateSize;
            }

            /* Apply iFFT to get the 256 (really 255) samples for output. The
             * 128 output samples are combined with the last output's 127
             * second-half samples (and this output's second half is
             * subsequently saved for next time).
             */
            pffft_transform(mFft.get(), mFftBuffer.data(), mFftBuffer.data(),
                mFftWorkBuffer.data(), PFFFT_BACKWARD);

            for(size_t i{0};i < ConvolveUpdateSamples;++i)
                mOutput[c][i] = mFftBuffer[i] + mOutput[c][ConvolveUpdateSamples+i];
            for(size_t i{0};i < ConvolveUpdateSamples;++i)
                mOutput[c][ConvolveUpdateSamples+i] = mFftBuffer[ConvolveUpdateSamples+i];
        }

        /* Shift the input history. */
        curseg = curseg ? (curseg-1) : (mNumConvolveSegs-1);
    }
    mCurrentSegment = curseg;

    /* Finally, mix to the output. */
    (this->*mMix)(samplesOut, samplesToDo);
}


struct ConvolutionStateFactory final : public EffectStateFactory {
    al::intrusive_ptr<EffectState> create() override
    { return al::intrusive_ptr<EffectState>{new ConvolutionState{}}; }
};

} // namespace

EffectStateFactory *ConvolutionStateFactory_getFactory()
{
    static ConvolutionStateFactory ConvolutionFactory{};
    return &ConvolutionFactory;
}
