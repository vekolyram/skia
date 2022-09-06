/*
 * Copyright 2014 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkImageFilter_Base.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFlattenable.h"
#include "include/core/SkRect.h"
#include "include/effects/SkImageFilters.h"
#include "src/core/SkImageFilterTypes.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkSamplingPriv.h"
#include "src/core/SkSpecialImage.h"
#include "src/core/SkSpecialSurface.h"
#include "src/core/SkWriteBuffer.h"

namespace {

class SkMatrixTransformImageFilter final : public SkImageFilter_Base {
public:
    // TODO(michaelludwig): Update this to use SkM44.
    SkMatrixTransformImageFilter(const SkMatrix& transform,
                                 const SkSamplingOptions& sampling,
                                 sk_sp<SkImageFilter> input)
            : SkImageFilter_Base(&input, 1, nullptr)
            , fTransform(transform)
            , fSampling(sampling) {
        // Pre-cache so future calls to fTransform.getType() are threadsafe.
        (void) static_cast<const SkMatrix&>(fTransform).getType();
    }

    SkRect computeFastBounds(const SkRect&) const override;

protected:
    void flatten(SkWriteBuffer&) const override;

private:
    friend void ::SkRegisterMatrixTransformImageFilterFlattenable();
    SK_FLATTENABLE_HOOKS(SkMatrixTransformImageFilter)

    skif::FilterResult onFilterImage(const skif::Context& context) const override;

    skif::LayerSpace<SkIRect> onGetInputLayerBounds(
            const skif::Mapping& mapping,
            const skif::LayerSpace<SkIRect>& desiredOutput,
            const skif::LayerSpace<SkIRect>& contentBounds,
            VisitChildren recurse) const override;

    skif::LayerSpace<SkIRect> onGetOutputLayerBounds(
            const skif::Mapping& mapping,
            const skif::LayerSpace<SkIRect>& contentBounds) const override;

    skif::ParameterSpace<SkMatrix> fTransform;
    SkSamplingOptions fSampling;
};

} // namespace

sk_sp<SkImageFilter> SkImageFilters::MatrixTransform(const SkMatrix& transform,
                                                     const SkSamplingOptions& sampling,
                                                     sk_sp<SkImageFilter> input) {
    return sk_sp<SkImageFilter>(new SkMatrixTransformImageFilter(transform,
                                                                 sampling,
                                                                 std::move(input)));
}

void SkRegisterMatrixTransformImageFilterFlattenable() {
    SK_REGISTER_FLATTENABLE(SkMatrixTransformImageFilter);
    // TODO(michaelludwig): Remove after grace period for SKPs to stop using old name
    SkFlattenable::Register("SkMatrixImageFilter", SkMatrixTransformImageFilter::CreateProc);
}

sk_sp<SkFlattenable> SkMatrixTransformImageFilter::CreateProc(SkReadBuffer& buffer) {
    SK_IMAGEFILTER_UNFLATTEN_COMMON(common, 1);
    SkMatrix matrix;
    buffer.readMatrix(&matrix);

    auto sampling = [&]() {
        if (buffer.isVersionLT(SkPicturePriv::kMatrixImageFilterSampling_Version)) {
            return SkSamplingPriv::FromFQ(buffer.read32LE(kLast_SkLegacyFQ), kLinear_SkMediumAs);
        } else {
            return buffer.readSampling();
        }
    }();
    return SkImageFilters::MatrixTransform(matrix, sampling, common.getInput(0));
}

void SkMatrixTransformImageFilter::flatten(SkWriteBuffer& buffer) const {
    this->SkImageFilter_Base::flatten(buffer);
    buffer.writeMatrix(SkMatrix(fTransform));
    buffer.writeSampling(fSampling);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

skif::FilterResult SkMatrixTransformImageFilter::onFilterImage(const skif::Context& context) const {
    skif::FilterResult childOutput = this->filterInput(0, context);
    skif::LayerSpace<SkMatrix> transform = context.mapping().paramToLayer(fTransform);
    return childOutput.applyTransform(context, transform, fSampling);
}

SkRect SkMatrixTransformImageFilter::computeFastBounds(const SkRect& src) const {
    SkRect bounds = this->getInput(0) ? this->getInput(0)->computeFastBounds(src) : src;
    return static_cast<const SkMatrix&>(fTransform).mapRect(bounds);
}

skif::LayerSpace<SkIRect> SkMatrixTransformImageFilter::onGetInputLayerBounds(
        const skif::Mapping& mapping,
        const skif::LayerSpace<SkIRect>& desiredOutput,
        const skif::LayerSpace<SkIRect>& contentBounds,
        VisitChildren recurse) const {
    // The required input for this filter to cover 'desiredOutput' is the smallest rectangle such
    // that after being transformed by the layer-space adjusted 'fTransform', it contains the output
    skif::LayerSpace<SkMatrix> inverse;
    if (!mapping.paramToLayer(fTransform).invert(&inverse)) {
        return skif::LayerSpace<SkIRect>::Empty();
    }
    skif::LayerSpace<SkIRect> requiredInput =
            inverse.mapRect(skif::LayerSpace<SkRect>(desiredOutput)).roundOut();

    // Additionally if there is any filtering beyond nearest neighbor, we request an extra buffer of
    // pixels so that the content is available to the bilerp/bicubic kernel.
    if (fSampling != SkSamplingOptions()) {
        requiredInput.outset(skif::LayerSpace<SkISize>({1, 1}));
    }

    if (recurse == VisitChildren::kNo) {
        return requiredInput;
    } else {
        // Our required input is the desired output for our child image filter.
        return this->visitInputLayerBounds(mapping, requiredInput, contentBounds);
    }
}

skif::LayerSpace<SkIRect> SkMatrixTransformImageFilter::onGetOutputLayerBounds(
        const skif::Mapping& mapping,
        const skif::LayerSpace<SkIRect>& contentBounds) const {
    // The output of this filter is the transformed bounds of its child's output.
    skif::LayerSpace<SkRect> childOutput{this->visitOutputLayerBounds(mapping, contentBounds)};
    return mapping.paramToLayer(fTransform).mapRect(childOutput).roundOut();
}
