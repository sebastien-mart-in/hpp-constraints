// Copyright (c) 2017, Joseph Mirabel
// Authors: Joseph Mirabel (joseph.mirabel@laas.fr)
//

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.

#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include <hpp/constraints/implicit.hh>
#include <hpp/constraints/macros.hh>
#include <hpp/constraints/solver/hierarchical-iterative.hh>
#include <hpp/constraints/solver/impl/hierarchical-iterative.hh>
#include <hpp/constraints/svd.hh>
#include <hpp/pinocchio/device.hh>
#include <hpp/pinocchio/joint-collection.hh>
#include <hpp/pinocchio/liegroup-element.hh>
#include <hpp/pinocchio/serialization.hh>
#include <hpp/pinocchio/util.hh>
#include <hpp/util/debug.hh>
#include <hpp/util/serialization.hh>
#include <hpp/util/timer.hh>
#include <limits>
#include <pinocchio/multibody/model.hpp>
#include <pinocchio/serialization/eigen.hpp>

// #define SVD_THRESHOLD Eigen::NumTraits<value_type>::dummy_precision()
#define SVD_THRESHOLD 1e-8

namespace hpp {
namespace constraints {
namespace solver {
namespace {
template <bool Superior, bool ComputeJac, typename Derived>
void compare(value_type& val, const Eigen::MatrixBase<Derived>& jac,
             const value_type& thr) {
  if ((Superior && val < thr) || (!Superior && -thr < val)) {
    if (Superior)
      val -= thr;
    else
      val += thr;
  } else {
    val = 0;
    if (ComputeJac)
      const_cast<Eigen::MatrixBase<Derived>&>(jac).derived().setZero();
  }
}

template <bool ComputeJac>
void applyComparison(const ComparisonTypes_t comparison,
                     const std::vector<std::size_t>& indices, vector_t& value,
                     matrix_t& jacobian, const value_type& thr) {
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const std::size_t j = indices[i];
    if (comparison[j] == Superior) {
      compare<true, ComputeJac>(value[j], jacobian.row(j), thr);
    } else if (comparison[j] == Inferior) {
      compare<false, ComputeJac>(value[j], jacobian.row(j), thr);
    }
  }
}
}  // namespace

namespace lineSearch {
template bool Constant::operator()(const HierarchicalIterative& solver,
                                   vectorOut_t arg, vectorOut_t darg);

Backtracking::Backtracking() : c(0.001), tau(0.7), smallAlpha(0.2) {}
template bool Backtracking::operator()(const HierarchicalIterative& solver,
                                       vectorOut_t arg, vectorOut_t darg);

FixedSequence::FixedSequence() : alpha(.2), alphaMax(.95), K(.8) {}
template bool FixedSequence::operator()(const HierarchicalIterative& solver,
                                        vectorOut_t arg, vectorOut_t darg);

ErrorNormBased::ErrorNormBased(value_type alphaMin, value_type _a,
                               value_type _b)
    : C(0.5 + alphaMin / 2), K((1 - alphaMin) / 2), a(_a), b(_b) {}

ErrorNormBased::ErrorNormBased(value_type alphaMin)
    : C(0.5 + alphaMin / 2), K((1 - alphaMin) / 2) {
  static const value_type delta = 0.02;
  static const value_type r_half = 1e6;

  a = atanh((delta - 1 + C) / K) / (1 - r_half);
  b = -r_half * a;
}

template bool ErrorNormBased::operator()(const HierarchicalIterative& solver,
                                         vectorOut_t arg, vectorOut_t darg);
}  // namespace lineSearch

namespace saturation {
bool Base::saturate(vectorIn_t q, vectorOut_t qSat,
                    Eigen::VectorXi& saturation) {
  qSat = q;
  saturation.setZero();
  return false;
}

bool clamp(const value_type& lb, const value_type& ub, const value_type& v,
           value_type& vsat, int& s) {
  if (v <= lb) {
    vsat = lb;
    s = -1;
    return true;
  } else if (v >= ub) {
    vsat = ub;
    s = 1;
    return true;
  } else {
    vsat = v;
    s = 0;
    return false;
  }
}

bool Bounds::saturate(vectorIn_t q, vectorOut_t qSat,
                      Eigen::VectorXi& saturation) {
  bool sat = false;
  for (size_type i = 0; i < q.size(); ++i)
    if (clamp(lb[i], ub[i], q[i], qSat[i], saturation[i])) sat = true;
  return sat;
}

bool Device::saturate(vectorIn_t q, vectorOut_t qSat, Eigen::VectorXi& sat) {
  bool ret = false;
  const pinocchio::Model& m = device->model();

  for (std::size_t i = 1; i < m.joints.size(); ++i) {
    const size_type nq = m.joints[i].nq();
    const size_type nv = m.joints[i].nv();
    const size_type idx_q = m.joints[i].idx_q();
    const size_type idx_v = m.joints[i].idx_v();
    for (size_type j = 0; j < nq; ++j) {
      const size_type iq = idx_q + j;
      const size_type iv = idx_v + std::min(j, nv - 1);
      if (clamp(m.lowerPositionLimit[iq], m.upperPositionLimit[iq], q[iq],
                qSat[iq], sat[iv]))
        ret = true;
    }
  }

  const hpp::pinocchio::ExtraConfigSpace& ecs = device->extraConfigSpace();
  const size_type& d = ecs.dimension();

  for (size_type k = 0; k < d; ++k) {
    const size_type iq = m.nq + k;
    const size_type iv = m.nv + k;
    if (clamp(ecs.lower(k), ecs.upper(k), q[iq], qSat[iq], sat[iv])) ret = true;
  }
  return ret;
}
}  // namespace saturation

typedef std::pair<DifferentiableFunctionPtr_t, size_type> iqIt_t;
typedef std::pair<DifferentiableFunctionPtr_t, std::size_t> priorityIt_t;

HierarchicalIterative::HierarchicalIterative(
    const LiegroupSpacePtr_t& configSpace)
    : squaredErrorThreshold_(0),
      inequalityThreshold_(0),
      maxIterations_(0),
      stacks_(),
      configSpace_(configSpace),
      dimension_(0),
      reducedDimension_(0),
      lastIsOptional_(false),
      solveLevelByLevel_(false),
      freeVariables_(),
      saturate_(new saturation::Base()),
      constraints_(),
      iq_(),
      iv_(),
      priority_(),
      sigma_(0),
      dq_(),
      dqSmall_(),
      reducedJ_(),
      saturation_(configSpace->nv()),
      reducedSaturation_(),
      qSat_(configSpace_->nq()),
      tmpSat_(),
      squaredNorm_(0),
      datas_(),
      svd_(),
      OM_(configSpace->nv()),
      OP_(configSpace->nv()) {
  // Initialize freeVariables_ to all indices.
  freeVariables_.addRow(0, configSpace_->nv());
}

HierarchicalIterative::HierarchicalIterative(const HierarchicalIterative& other)
    : squaredErrorThreshold_(other.squaredErrorThreshold_),
      inequalityThreshold_(other.inequalityThreshold_),
      maxIterations_(other.maxIterations_),
      stacks_(other.stacks_),
      configSpace_(other.configSpace_),
      dimension_(other.dimension_),
      reducedDimension_(other.reducedDimension_),
      lastIsOptional_(other.lastIsOptional_),
      freeVariables_(other.freeVariables_),
      saturate_(other.saturate_),
      constraints_(other.constraints_.size()),
      iq_(other.iq_),
      iv_(other.iv_),
      priority_(other.priority_),
      sigma_(other.sigma_),
      dq_(other.dq_),
      dqSmall_(other.dqSmall_),
      reducedJ_(other.reducedJ_),
      saturation_(other.saturation_),
      reducedSaturation_(other.reducedSaturation_),
      qSat_(other.qSat_),
      tmpSat_(other.tmpSat_),
      squaredNorm_(other.squaredNorm_),
      datas_(other.datas_),
      svd_(other.svd_),
      OM_(other.OM_),
      OP_(other.OP_) {
  for (std::size_t i = 0; i < constraints_.size(); ++i)
    constraints_[i] = other.constraints_[i]->copy();
}

bool HierarchicalIterative::contains(
    const ImplicitPtr_t& numericalConstraint) const {
  return find_if(constraints_.begin(), constraints_.end(),
                 [&numericalConstraint](const ImplicitPtr_t& arg) {
                   return *arg == *numericalConstraint;
                 }) != constraints_.end();
}

bool HierarchicalIterative::add(const ImplicitPtr_t& constraint,
                                const std::size_t& priority) {
  DifferentiableFunctionPtr_t f(constraint->functionPtr());
  if (find_if(priority_.begin(), priority_.end(),
              [&f](const priorityIt_t& arg) { return *arg.first == *f; }) !=
      priority_.end()) {
    std::ostringstream oss;
    oss << "Contraint \"" << f->name() << "\" already in solver";
    throw std::logic_error(oss.str().c_str());
  }
  priority_[f] = priority;
  const ComparisonTypes_t comp(constraint->comparisonType());
  assert((size_type)comp.size() == f->outputDerivativeSize());
  const std::size_t minSize = priority + 1;
  if (stacks_.size() < minSize) {
    stacks_.resize(minSize, ImplicitConstraintSet());
    datas_.resize(minSize, Data());
  }
  Data& d = datas_[priority];
  // Store rank in output vector value
  iq_[f] = datas_[priority].output.space()->nq();
  // Store rank in output vector derivative
  iv_[f] = datas_[priority].output.space()->nv();
  // warning adding constraint to the stack modifies behind the stage
  // the dimension of datas_ [priority].output.space (). It should
  // therefore be done after the previous lines.
  stacks_[priority].add(constraint);
  for (std::size_t i = 0; i < comp.size(); ++i) {
    if ((comp[i] == Superior) || (comp[i] == Inferior)) {
      d.inequalityIndices.push_back(d.comparison.size());
    } else if (comp[i] == Equality) {
      d.equalityIndices.addRow(d.comparison.size(), 1);
    }
    d.comparison.push_back(comp[i]);
  }
  d.equalityIndices.updateRows<true, true, true>();
  constraints_.push_back(constraint);
  update();

  return true;
}

void HierarchicalIterative::merge(const HierarchicalIterative& other) {
  std::size_t priority;
  for (NumericalConstraints_t::const_iterator it(other.constraints_.begin());
       it != other.constraints_.end(); ++it) {
    if (!this->contains(*it)) {
      const DifferentiableFunctionPtr_t& f = (*it)->functionPtr();
      std::map<DifferentiableFunctionPtr_t, std::size_t>::const_iterator itp(
          std::find_if(
              other.priority_.begin(), other.priority_.end(),
              [&f](const priorityIt_t& arg) { return *arg.first == *f; }));
      if (itp == other.priority_.end()) {
        // If priority is not set, constraint is explicit
        priority = 0;
      } else {
        priority = itp->second;
      }
      this->add(*it, priority);
    }
  }
}

ArrayXb HierarchicalIterative::activeParameters() const {
  ArrayXb ap(ArrayXb::Constant(configSpace_->nq(), false));
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
#ifndef NDEBUG
    dynamic_cast<const DifferentiableFunctionSet&>(stacks_[i].function());
#endif
    const DifferentiableFunctionSet& dfs(
        dynamic_cast<const DifferentiableFunctionSet&>(stacks_[i].function()));
    ap = ap || dfs.activeParameters();
  }
  return ap;
}

ArrayXb HierarchicalIterative::activeDerivativeParameters() const {
  ArrayXb ap(ArrayXb::Constant(configSpace_->nv(), false));
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
#ifndef NDEBUG
    dynamic_cast<const DifferentiableFunctionSet&>(stacks_[i].function());
#endif
    const DifferentiableFunctionSet& dfs(
        dynamic_cast<const DifferentiableFunctionSet&>(stacks_[i].function()));
    ap = ap || dfs.activeDerivativeParameters();
  }
  return ap;
}

void HierarchicalIterative::update() {
  // Compute reduced size
  std::size_t reducedSize = freeVariables_.nbIndices();

  dimension_ = 0;
  reducedDimension_ = 0;
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    computeActiveRowsOfJ(i);

    const ImplicitConstraintSet& constraints(stacks_[i]);
#ifndef NDEBUG
    dynamic_cast<const DifferentiableFunctionSet&>(constraints.function());
#endif
    const DifferentiableFunctionSet& f(
        static_cast<const DifferentiableFunctionSet&>(constraints.function()));
    dimension_ += f.outputDerivativeSize();
    reducedDimension_ += datas_[i].activeRowsOfJ.nbRows();
    datas_[i].output = LiegroupElement(f.outputSpace());
    datas_[i].rightHandSide = LiegroupElement(f.outputSpace());
    datas_[i].rightHandSide.setNeutral();
    datas_[i].error.resize(f.outputSpace()->nv());

    assert(configSpace_->nv() == f.inputDerivativeSize());
    datas_[i].jacobian.resize(f.outputDerivativeSize(),
                              f.inputDerivativeSize());
    datas_[i].jacobian.setZero();
    datas_[i].reducedJ.resize(datas_[i].activeRowsOfJ.nbRows(), reducedSize);

    datas_[i].svd = SVD_t(
        f.outputDerivativeSize(), reducedSize,
        Eigen::ComputeThinU | (i == stacks_.size() - 1 ? Eigen::ComputeThinV
                                                       : Eigen::ComputeFullV));
    datas_[i].svd.setThreshold(SVD_THRESHOLD);
    datas_[i].PK.resize(reducedSize, reducedSize);

    datas_[i].maxRank = 0;
  }

  dq_ = vector_t::Zero(configSpace_->nv());
  dqSmall_.resize(reducedSize);
  reducedJ_.resize(reducedDimension_, reducedSize);
  svd_ = SVD_t(reducedDimension_, reducedSize,
               Eigen::ComputeThinU | Eigen::ComputeThinV);
}

void HierarchicalIterative::computeActiveRowsOfJ(std::size_t iStack) {
  Data& d = datas_[iStack];
  const ImplicitConstraintSet::Implicits_t constraints(
      stacks_[iStack].constraints());
  std::size_t offset = 0;

  typedef Eigen::MatrixBlocks<false, false> BlockIndices;
  BlockIndices::segments_t rows;
  // Loop over functions of the stack
  for (std::size_t i = 0; i < constraints.size(); ++i) {
    ArrayXb adp = freeVariables_
                      .rview(constraints[i]
                                 ->function()
                                 .activeDerivativeParameters()
                                 .matrix())
                      .eval();
    if (adp.any())  // If at least one element of adp is true
      for (const segment_t s : constraints[i]->activeRows()) {
        rows.emplace_back(s.first + offset, s.second);
      }
    offset += constraints[i]->function().outputDerivativeSize();
  }
  d.activeRowsOfJ =
      Eigen::MatrixBlocks<false, false>(rows, freeVariables_.m_rows);
  d.activeRowsOfJ.updateRows<true, true, true>();
}

vector_t HierarchicalIterative::rightHandSideFromConfig(
    ConfigurationIn_t config) {
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    ImplicitConstraintSet& ics = stacks_[i];
    Data& d = datas_[i];
    ics.rightHandSideFromConfig(config, d.rightHandSide);
  }
  return rightHandSide();
}

bool HierarchicalIterative::rightHandSideFromConfig(
    const ImplicitPtr_t& constraint, ConfigurationIn_t config) {
  const DifferentiableFunctionPtr_t& f(constraint->functionPtr());

  std::map<DifferentiableFunctionPtr_t, size_type>::iterator iqi(
      find_if(iq_.begin(), iq_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  if (iqi == iq_.end()) return false;
  LiegroupSpacePtr_t space(f->outputSpace());
  size_type iq = iqi->second;

  std::map<DifferentiableFunctionPtr_t, std::size_t>::iterator prioi(
      find_if(priority_.begin(), priority_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  if (prioi == priority_.end()) return false;
  std::size_t i = prioi->second;

  size_type nq = space->nq();
  Data& d = datas_[i];
  LiegroupElementRef rhs(
      space->elementRef(d.rightHandSide.vector().segment(iq, nq)));
  constraint->rightHandSideFromConfig(config, rhs);
  return true;
}

bool HierarchicalIterative::rightHandSide(const ImplicitPtr_t& constraint,
                                          vectorIn_t rightHandSide) {
  const DifferentiableFunctionPtr_t& f(constraint->functionPtr());
  LiegroupSpacePtr_t space(f->outputSpace());
  assert(rightHandSide.size() == space->nq());

  std::map<DifferentiableFunctionPtr_t, size_type>::iterator iqi(
      find_if(iq_.begin(), iq_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  if (iqi == iq_.end()) return false;
  size_type iq = iqi->second;
  size_type nq = space->nq();

  std::map<DifferentiableFunctionPtr_t, std::size_t>::iterator prioi(
      find_if(priority_.begin(), priority_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  if (prioi == priority_.end()) return false;
  std::size_t i = prioi->second;

  Data& d = datas_[i];
#ifndef NDEBUG
  size_type nv = space->nv();
  assert(d.error.size() >= nv);
#endif
  pinocchio::LiegroupElementConstRef inRhs(
      space->elementConstRef(rightHandSide));
  LiegroupElementRef rhs(
      space->elementRef(d.rightHandSide.vector().segment(iq, nq)));
  rhs = inRhs;
  assert(constraint->checkRightHandSide(inRhs));
  return true;
}

bool HierarchicalIterative::getRightHandSide(const ImplicitPtr_t& constraint,
                                             vectorOut_t rightHandSide) const {
  const DifferentiableFunctionPtr_t& f(constraint->functionPtr());
  std::map<DifferentiableFunctionPtr_t, std::size_t>::const_iterator itp(
      find_if(priority_.begin(), priority_.end(),
              [&f](const priorityIt_t& arg) { return *arg.first == *f; }));
  if (itp == priority_.end()) {
    return false;
  }
  std::map<DifferentiableFunctionPtr_t, size_type>::const_iterator itIq(
      find_if(iq_.begin(), iq_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  if (itIq == iq_.end()) {
    return false;
  }
  LiegroupSpacePtr_t space(f->outputSpace());
  std::size_t i = itp->second;
  size_type iq = itIq->second;
  Data& d = datas_[i];
  assert(rightHandSide.size() == space->nq());
  assert(d.rightHandSide.space()->nq() >= iq + space->nq());
  rightHandSide = d.rightHandSide.vector().segment(iq, space->nq());
  return true;
}

bool HierarchicalIterative::isConstraintSatisfied(
    const ImplicitPtr_t& constraint, vectorIn_t arg, vectorOut_t error,
    bool& constraintFound) const {
  const DifferentiableFunctionPtr_t& f(constraint->functionPtr());
  assert(error.size() == f->outputSpace()->nv());
  std::map<DifferentiableFunctionPtr_t, std::size_t>::const_iterator itp(
      find_if(priority_.begin(), priority_.end(),
              [&f](const priorityIt_t& arg) { return *arg.first == *f; }));
  if (itp == priority_.end()) {
    constraintFound = false;
    return false;
  }
  constraintFound = true;
  std::map<DifferentiableFunctionPtr_t, size_type>::const_iterator itIq(
      find_if(iq_.begin(), iq_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  assert(itIq != iq_.end());
  std::map<DifferentiableFunctionPtr_t, size_type>::const_iterator itIv(
      find_if(iv_.begin(), iv_.end(),
              [&f](const iqIt_t& arg) { return *arg.first == *f; }));
  assert(itIv != iv_.end());
  size_type priority(itp->second);
  Data& d = datas_[priority];
  // Evaluate constraint function
  size_type iq = itIq->second, nq = f->outputSpace()->nq();
  LiegroupElementRef output(d.output.vector().segment(iq, nq),
                            f->outputSpace());
  LiegroupElementRef rhs(d.rightHandSide.vector().segment(iq, nq),
                         f->outputSpace());
  f->value(output, arg);
  error = output - rhs;
  constraint->setInactiveRowsToZero(error);
  return (error.squaredNorm() < squaredErrorThreshold_);
}

void HierarchicalIterative::rightHandSide(vectorIn_t rightHandSide) {
  size_type iq = 0, iv = 0;
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    Data& d = datas_[i];
    LiegroupSpacePtr_t space(d.rightHandSide.space());
    size_type nq = space->nq();
    size_type nv = space->nv();
    pinocchio::LiegroupElementConstRef output(
        space->elementConstRef(rightHandSide.segment(iq, nq)));
    LiegroupElementRef rhs(
        space->elementRef(d.rightHandSide.vector().segment(iq, nq)));

    // d.error is used here as an intermediate storage. The value
    // computed is not the error
    d.error = output - space->neutral();  // log (rightHandSide)
    for (size_type k = 0; k < nv; ++k) {
      if (d.comparison[iv + k] != Equality) assert(d.error[k] == 0);
    }
    rhs = space->neutral() + d.error;  // exp (d.error)
    iq += nq;
    iv += nv;
  }
  assert(iq == rightHandSide.size());
}

void HierarchicalIterative::rightHandSideAt(const value_type& s) {
  for (std::size_t i = 0; i < constraints_.size(); ++i) {
    ImplicitPtr_t implicit = constraints_[i];
    // If constraint has no right hand side function set, do nothing
    if ((implicit->parameterSize() != 0) &&
        (implicit->rightHandSideFunction())) {
      rightHandSide(implicit, implicit->rightHandSideAt(s));
    }
  }
}

vector_t HierarchicalIterative::rightHandSide() const {
  vector_t rhs(rightHandSideSize());
  size_type iq = 0;
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    const Data& d = datas_[i];
    size_type nq = d.rightHandSide.space()->nq();
    // this does not take the comparison type into account.
    // It shouldn't matter as rhs should be zero when comparison type is
    // not Equality
    rhs.segment(iq, nq) = d.rightHandSide.vector();
    iq += nq;
  }
  assert(iq == rhs.size());
  return rhs;
}

size_type HierarchicalIterative::rightHandSideSize() const {
  size_type rhsSize = 0;
  for (std::size_t i = 0; i < stacks_.size(); ++i)
    rhsSize += stacks_[i].function().outputSize();
  return rhsSize;
}

template <bool ComputeJac>
void HierarchicalIterative::computeValue(vectorIn_t config) const {
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    const ImplicitConstraintSet& constraints(stacks_[i]);
    const DifferentiableFunction& f = constraints.function();
    Data& d = datas_[i];

    f.value(d.output, config);
    assert(hpp::pinocchio::checkNormalized(d.output));
    assert(hpp::pinocchio::checkNormalized(d.rightHandSide));
    d.error = d.output - d.rightHandSide;
    constraints.setInactiveRowsToZero(d.error);
    if (ComputeJac) {
      f.jacobian(d.jacobian, config);
      d.output.space()->dDifference_dq1<pinocchio::DerivativeTimesInput>(
          d.rightHandSide.vector(), d.output.vector(), d.jacobian);
    }
    applyComparison<ComputeJac>(d.comparison, d.inequalityIndices, d.error,
                                d.jacobian, inequalityThreshold_);

    // Copy columns that are not reduced
    if (ComputeJac) d.reducedJ = d.activeRowsOfJ.rview(d.jacobian);
  }
}

template void HierarchicalIterative::computeValue<false>(
    vectorIn_t config) const;
template void HierarchicalIterative::computeValue<true>(
    vectorIn_t config) const;

void HierarchicalIterative::computeSaturation(vectorIn_t config) const {
  bool applySaturate = saturate_->saturate(config, qSat_, saturation_);
  if (!applySaturate) return;

  reducedSaturation_ = freeVariables_.rview(saturation_);
  assert((reducedSaturation_.array() == -1 || reducedSaturation_.array() == 0 ||
          reducedSaturation_.array() == 1)
             .all());

  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    Data& d = datas_[i];

    vector_t error = d.activeRowsOfJ.keepRows().rview(d.error);
    tmpSat_ = (reducedSaturation_.cast<value_type>()
                   .cwiseProduct(d.reducedJ.transpose() * error)
                   .array() < 0);
    for (size_type j = 0; j < tmpSat_.size(); ++j)
      if (tmpSat_[j]) d.reducedJ.col(j).setZero();
  }
}

void HierarchicalIterative::getValue(vectorOut_t v) const {
  size_type row = 0;
  for (std::size_t i = 0; i < datas_.size(); ++i) {
    const Data& d = datas_[i];
    v.segment(row, d.output.vector().rows()) = d.output.vector();
    row += d.output.vector().rows();
  }
  assert(v.rows() == row);
}

void HierarchicalIterative::getReducedJacobian(matrixOut_t J) const {
  size_type row = 0;
  for (std::size_t i = 0; i < datas_.size(); ++i) {
    const Data& d = datas_[i];
    J.middleRows(row, d.reducedJ.rows()) = d.reducedJ;
    row += d.reducedJ.rows();
  }
  assert(J.rows() == row);
}

void HierarchicalIterative::computeError() const {
  const std::size_t end =
      (lastIsOptional_ ? stacks_.size() - 1 : stacks_.size());
  squaredNorm_ = 0;
  for (std::size_t i = 0; i < end; ++i) {
    const ImplicitConstraintSet::Implicits_t constraints(
        stacks_[i].constraints());
    const Data& d = datas_[i];
    size_type iv = 0;
    for (std::size_t j = 0; j < constraints.size(); ++j) {
      size_type nv(constraints[j]->function().outputDerivativeSize());
      squaredNorm_ =
          std::max(squaredNorm_, d.error.segment(iv, nv).squaredNorm());
      iv += nv;
    }
  }
}

bool HierarchicalIterative::integrate(vectorIn_t from, vectorIn_t velocity,
                                      vectorOut_t result) const {
  typedef pinocchio::LiegroupElementRef LgeRef_t;
  result = from;
  LgeRef_t M(result, configSpace_);
  M += velocity;
  return saturate_->saturate(result, result, saturation_);
}

void HierarchicalIterative::residualError(vectorOut_t error) const {
  size_type row = 0;
  for (std::size_t i = 0; i < datas_.size(); ++i) {
    const Data& d = datas_[i];
    error.segment(row, d.error.size()) = d.error;
    row += d.error.size();
  }
}

bool HierarchicalIterative::definesSubmanifoldOf(
    const HierarchicalIterative& solver) const {
  for (NumericalConstraints_t::const_iterator it(solver.constraints().begin());
       it != solver.constraints().end(); ++it) {
    const DifferentiableFunctionPtr_t& f = (*it)->functionPtr();
    if (find_if(constraints_.begin(), constraints_.end(),
                [&f](const ImplicitPtr_t& arg) {
                  return arg->function() == *f;
                }) == constraints_.end())
      return false;
  }
  return true;
}

void HierarchicalIterative::computeDescentDirection() const {
  sigma_ = std::numeric_limits<value_type>::max();

  if (stacks_.empty()) {
    dq_.setZero();
    return;
  }
  vector_t err;
  if (stacks_.size() == 1) {  // one level only
    Data& d = datas_[0];
    d.svd.compute(d.reducedJ);
    HPP_DEBUG_SVDCHECK(d.svd);
    // TODO Eigen::JacobiSVD does a dynamic allocation here.
    err = d.activeRowsOfJ.keepRows().rview(-d.error);
    dqSmall_ = d.svd.solve(err);
    d.maxRank = std::max(d.maxRank, d.svd.rank());
    if (d.maxRank > 0)
      sigma_ = std::min(sigma_, d.svd.singularValues()[d.maxRank - 1]);
  } else {
    // dq = dQ_0 + P_0 * v_1
    // f_1(q+dq) = f_1(q) + J_1 * dQ_0 + M_1 * v_1
    // M_1 = J_1 * P_0
    // v_1 = M+_1 * (-f_1(q) - J_1 * dQ_1) + K_1 * v_2
    // dq = dQ_0 + P_0 * M+_1 * (-f_1(q) - J_1 * dQ_1) + P_0 * K_1 * v_2
    //    = dQ_1                                       + P_1       * b_2
    //
    // dQ_1 = dQ_0 + P_0 * M+_1 * (-f_1(q) - J_1 * dQ_1)
    //  P_1 = P_0 * K_1
    matrix_t* projector = NULL;
    for (std::size_t i = 0; i < stacks_.size(); ++i) {
      Data& d = datas_[i];

      // TODO: handle case where this is the first element of the stack and it
      // has no functions
      if (d.reducedJ.rows() == 0) continue;
      /// projector is of size numberDof
      bool first = (i == 0);
      bool last = (i == stacks_.size() - 1);
      if (first) {
        err = d.activeRowsOfJ.keepRows().rview(-d.error);
        // dq should be zero and projector should be identity
        d.svd.compute(d.reducedJ);
        HPP_DEBUG_SVDCHECK(d.svd);
        // TODO Eigen::JacobiSVD does a dynamic allocation here.
        dqSmall_ = d.svd.solve(err);
      } else {
        err = d.activeRowsOfJ.keepRows().rview(-d.error);
        err.noalias() -= d.reducedJ * dqSmall_;

        if (projector == NULL) {
          d.svd.compute(d.reducedJ);
          // TODO Eigen::JacobiSVD does a dynamic allocation here.
          dqSmall_ += d.svd.solve(err);
        } else {
          d.svd.compute(d.reducedJ * *projector);
          // TODO Eigen::JacobiSVD does a dynamic allocation here.
          dqSmall_ += *projector * d.svd.solve(err);
        }
        HPP_DEBUG_SVDCHECK(d.svd);
      }
      // Update sigma
      const size_type rank = d.svd.rank();
      d.maxRank = std::max(d.maxRank, rank);
      if (d.maxRank > 0)
        sigma_ = std::min(sigma_, d.svd.singularValues()[d.maxRank - 1]);
      if (solveLevelByLevel_ && err.squaredNorm() > squaredErrorThreshold_)
        break;
      if (last) break;  // No need to compute projector for next step.

      if (d.svd.matrixV().cols() == rank) break;  // The kernel is { 0 }
      /// compute projector for next step.
      if (projector == NULL)
        d.PK.noalias() = getV2<SVD_t>(d.svd, rank);
      else
        d.PK.noalias() = *projector * getV2<SVD_t>(d.svd, rank);
      projector = &d.PK;
    }
  }
  expandDqSmall();
}

void HierarchicalIterative::expandDqSmall() const {
  Eigen::MatrixBlockView<vector_t, Eigen::Dynamic, 1, false, true>(
      dq_, freeVariables_.nbIndices(), freeVariables_.indices()) = dqSmall_;
}

std::ostream& HierarchicalIterative::print(std::ostream& os) const {
  os << "HierarchicalIterative, " << stacks_.size() << " level." << iendl
     << "max iter: " << maxIterations()
     << ", error threshold: " << errorThreshold() << iendl << "dimension "
     << dimension() << iendl << "reduced dimension " << reducedDimension()
     << iendl << "free variables: " << freeVariables_ << incindent;
  const std::size_t end =
      (lastIsOptional_ ? stacks_.size() - 1 : stacks_.size());
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    const ImplicitConstraintSet::Implicits_t constraints(
        stacks_[i].constraints());
    const Data& d = datas_[i];
    os << iendl << "Level " << i;
    if (lastIsOptional_ && i == end) os << " (optional)";
    os << ": Stack of " << constraints.size() << " functions" << incindent;
    size_type rv = 0, rq = 0;
    for (std::size_t j = 0; j < constraints.size(); ++j) {
      const DifferentiableFunctionPtr_t& f(constraints[j]->functionPtr());
      os << iendl << j << ": [" << rv << ", " << f->outputDerivativeSize()
         << "]," << incindent << *f << iendl << "Rhs: "
         << condensed(d.rightHandSide.vector().segment(rq, f->outputSize()))
         << iendl << "active rows: " << condensed(constraints[j]->activeRows())
         << decindent;
      rv += f->outputDerivativeSize();
      rq += f->outputSize();
    }
    os << decendl;
    os << "Equality idx: " << d.equalityIndices;
    os << iendl << "Active rows: " << d.activeRowsOfJ;
  }
  return os << decindent;
}

template HierarchicalIterative::Status HierarchicalIterative::solve(
    vectorOut_t arg, lineSearch::Constant lineSearch) const;
template HierarchicalIterative::Status HierarchicalIterative::solve(
    vectorOut_t arg, lineSearch::Backtracking lineSearch) const;
template HierarchicalIterative::Status HierarchicalIterative::solve(
    vectorOut_t arg, lineSearch::FixedSequence lineSearch) const;
template HierarchicalIterative::Status HierarchicalIterative::solve(
    vectorOut_t arg, lineSearch::ErrorNormBased lineSearch) const;

template <class Archive>
void HierarchicalIterative::load(Archive& ar, const unsigned int version) {
  (void)version;
  ar& BOOST_SERIALIZATION_NVP(squaredErrorThreshold_);
  ar& BOOST_SERIALIZATION_NVP(inequalityThreshold_);
  ar& BOOST_SERIALIZATION_NVP(maxIterations_);
  ar& BOOST_SERIALIZATION_NVP(configSpace_);
  ar& BOOST_SERIALIZATION_NVP(lastIsOptional_);
  ar& BOOST_SERIALIZATION_NVP(saturate_);

  saturation_.resize(configSpace_->nq());
  qSat_.resize(configSpace_->nq());
  OM_.resize(configSpace_->nv());
  OP_.resize(configSpace_->nv());
  // Initialize freeVariables_ to all indices.
  freeVariables_.addRow(0, configSpace_->nv());

  NumericalConstraints_t constraints;
  std::vector<std::size_t> priorities;
  ar& boost::serialization::make_nvp("constraints_", constraints);
  ar& BOOST_SERIALIZATION_NVP(priorities);

  for (std::size_t i = 0; i < constraints.size(); ++i)
    add(constraints[i], priorities[i]);
  // TODO load the right hand side.
}

template <class Archive>
void HierarchicalIterative::save(Archive& ar,
                                 const unsigned int version) const {
  (void)version;
  ar& BOOST_SERIALIZATION_NVP(squaredErrorThreshold_);
  ar& BOOST_SERIALIZATION_NVP(inequalityThreshold_);
  ar& BOOST_SERIALIZATION_NVP(maxIterations_);
  ar& BOOST_SERIALIZATION_NVP(configSpace_);
  ar& BOOST_SERIALIZATION_NVP(lastIsOptional_);
  ar& BOOST_SERIALIZATION_NVP(saturate_);
  ar& BOOST_SERIALIZATION_NVP(constraints_);
  std::vector<std::size_t> priorities(constraints_.size());
  for (std::size_t i = 0; i < constraints_.size(); ++i) {
    const DifferentiableFunctionPtr_t& f = constraints_[i]->functionPtr();
    std::map<DifferentiableFunctionPtr_t, std::size_t>::const_iterator c(
        find_if(priority_.begin(), priority_.end(),
                [&f](const priorityIt_t& arg) { return *arg.first == *f; }));
    if (c == priority_.end())
      priorities[i] = 0;
    else
      priorities[i] = c->second;
  }
  ar& BOOST_SERIALIZATION_NVP(priorities);
  // TODO save the right hand side.
}

HPP_SERIALIZATION_SPLIT_IMPLEMENT(HierarchicalIterative);
}  // namespace solver
}  // namespace constraints
}  // namespace hpp

BOOST_CLASS_EXPORT(hpp::constraints::solver::saturation::Bounds)
BOOST_CLASS_EXPORT(hpp::constraints::solver::saturation::Device)

namespace boost {
namespace serialization {
using namespace hpp::constraints::solver;

template <class Archive>
void serialize(Archive&, saturation::Base&, const unsigned int) {}
template <class Archive>
void serialize(Archive& ar, saturation::Device& o, const unsigned int version) {
  (void)version;
  ar& make_nvp("base", base_object<saturation::Base>(o));
  ar& make_nvp("device", o.device);
}
template <class Archive>
void serialize(Archive& ar, saturation::Bounds& o, const unsigned int version) {
  (void)version;
  ar& make_nvp("base", base_object<saturation::Base>(o));
  hpp::serialization::remove_duplicate::serialize_vector(ar, "lb", o.lb,
                                                         version);
  hpp::serialization::remove_duplicate::serialize_vector(ar, "ub", o.ub,
                                                         version);
}
}  // namespace serialization
}  // namespace boost
