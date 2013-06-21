/* a*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SVGFEConvolveMatrixElement_h
#define mozilla_dom_SVGFEConvolveMatrixElement_h

#include "nsSVGBoolean.h"
#include "nsSVGEnum.h"
#include "nsSVGFilters.h"
#include "nsSVGInteger.h"
#include "nsSVGIntegerPair.h"
#include "nsSVGNumber2.h"
#include "nsSVGString.h"
#include "SVGAnimatedNumberList.h"

nsresult NS_NewSVGFEConvolveMatrixElement(nsIContent **aResult,
                                          already_AddRefed<nsINodeInfo> aNodeInfo);

namespace mozilla {
class DOMSVGAnimatedNumberList;

namespace dom {
class SVGAnimatedBoolean;

typedef nsSVGFE SVGFEConvolveMatrixElementBase;

class SVGFEConvolveMatrixElement : public SVGFEConvolveMatrixElementBase
{
  friend nsresult (::NS_NewSVGFEConvolveMatrixElement(nsIContent **aResult,
                                                      already_AddRefed<nsINodeInfo> aNodeInfo));
protected:
  SVGFEConvolveMatrixElement(already_AddRefed<nsINodeInfo> aNodeInfo)
    : SVGFEConvolveMatrixElementBase(aNodeInfo)
  {
  }
  virtual JSObject* WrapNode(JSContext* aCx,
                             JS::Handle<JSObject*> aScope) MOZ_OVERRIDE;

public:
  virtual nsresult Filter(nsSVGFilterInstance* aInstance,
                          const nsTArray<const Image*>& aSources,
                          const Image* aTarget,
                          const nsIntRect& aDataRect) MOZ_OVERRIDE;
  virtual bool AttributeAffectsRendering(
          int32_t aNameSpaceID, nsIAtom* aAttribute) const MOZ_OVERRIDE;
  virtual nsSVGString& GetResultImageName() MOZ_OVERRIDE { return mStringAttributes[RESULT]; }
  virtual void GetSourceImageNames(nsTArray<nsSVGStringInfo>& aSources) MOZ_OVERRIDE;
  virtual nsIntRect ComputeTargetBBox(const nsTArray<nsIntRect>& aSourceBBoxes,
          const nsSVGFilterInstance& aInstance) MOZ_OVERRIDE;
  virtual void ComputeNeededSourceBBoxes(const nsIntRect& aTargetBBox,
          nsTArray<nsIntRect>& aSourceBBoxes, const nsSVGFilterInstance& aInstance) MOZ_OVERRIDE;
  virtual nsIntRect ComputeChangeBBox(const nsTArray<nsIntRect>& aSourceChangeBoxes,
          const nsSVGFilterInstance& aInstance) MOZ_OVERRIDE;


  virtual nsresult Clone(nsINodeInfo *aNodeInfo, nsINode **aResult) const MOZ_OVERRIDE;

  // WebIDL
  already_AddRefed<SVGAnimatedString> In1();
  already_AddRefed<nsIDOMSVGAnimatedInteger> OrderX();
  already_AddRefed<nsIDOMSVGAnimatedInteger> OrderY();
  already_AddRefed<DOMSVGAnimatedNumberList> KernelMatrix();
  already_AddRefed<nsIDOMSVGAnimatedInteger> TargetX();
  already_AddRefed<nsIDOMSVGAnimatedInteger> TargetY();
  already_AddRefed<nsIDOMSVGAnimatedEnumeration> EdgeMode();
  already_AddRefed<SVGAnimatedBoolean> PreserveAlpha();
  already_AddRefed<nsIDOMSVGAnimatedNumber> Divisor();
  already_AddRefed<nsIDOMSVGAnimatedNumber> Bias();
  already_AddRefed<nsIDOMSVGAnimatedNumber> KernelUnitLengthX();
  already_AddRefed<nsIDOMSVGAnimatedNumber> KernelUnitLengthY();

protected:
  virtual bool OperatesOnPremultipledAlpha(int32_t) MOZ_OVERRIDE {
    return !mBooleanAttributes[PRESERVEALPHA].GetAnimValue();
  }

  virtual NumberAttributesInfo GetNumberInfo() MOZ_OVERRIDE;
  virtual NumberPairAttributesInfo GetNumberPairInfo() MOZ_OVERRIDE;
  virtual IntegerAttributesInfo GetIntegerInfo() MOZ_OVERRIDE;
  virtual IntegerPairAttributesInfo GetIntegerPairInfo() MOZ_OVERRIDE;
  virtual BooleanAttributesInfo GetBooleanInfo() MOZ_OVERRIDE;
  virtual EnumAttributesInfo GetEnumInfo() MOZ_OVERRIDE;
  virtual StringAttributesInfo GetStringInfo() MOZ_OVERRIDE;
  virtual NumberListAttributesInfo GetNumberListInfo() MOZ_OVERRIDE;

  enum { DIVISOR, BIAS };
  nsSVGNumber2 mNumberAttributes[2];
  static NumberInfo sNumberInfo[2];

  enum { KERNEL_UNIT_LENGTH };
  nsSVGNumberPair mNumberPairAttributes[1];
  static NumberPairInfo sNumberPairInfo[1];

  enum { TARGET_X, TARGET_Y };
  nsSVGInteger mIntegerAttributes[2];
  static IntegerInfo sIntegerInfo[2];

  enum { ORDER };
  nsSVGIntegerPair mIntegerPairAttributes[1];
  static IntegerPairInfo sIntegerPairInfo[1];

  enum { PRESERVEALPHA };
  nsSVGBoolean mBooleanAttributes[1];
  static BooleanInfo sBooleanInfo[1];

  enum { EDGEMODE };
  nsSVGEnum mEnumAttributes[1];
  static nsSVGEnumMapping sEdgeModeMap[];
  static EnumInfo sEnumInfo[1];

  enum { RESULT, IN1 };
  nsSVGString mStringAttributes[2];
  static StringInfo sStringInfo[2];

  enum { KERNELMATRIX };
  SVGAnimatedNumberList mNumberListAttributes[1];
  static NumberListInfo sNumberListInfo[1];
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_SVGFEConvolveMatrixElement_h
