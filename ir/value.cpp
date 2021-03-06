// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "ir/instr.h"
#include "ir/globals.h"
#include "ir/value.h"
#include "smt/expr.h"
#include "util/compiler.h"
#include "util/config.h"
#include <sstream>

using namespace smt;
using namespace std;
using namespace util;

namespace IR {

VoidValue Value::voidVal;

bool Value::isVoid() const {
  return &getType() == &Type::voidTy;
}

expr Value::getTypeConstraints() const {
  return getType().getTypeConstraints();
}

void Value::fixupTypes(const Model &m) {
  type.fixup(m);
}

ostream& operator<<(ostream &os, const Value &val) {
  auto t = val.getType().toString();
  os << t;
  if (!val.isVoid()) {
    if (!t.empty()) os << ' ';
    os << val.getName();
  }
  return os;
}


void UndefValue::print(ostream &os) const {
  UNREACHABLE();
}

StateValue UndefValue::toSMT(State &s) const {
  auto val = getType().getDummyValue(true);
  expr var = expr::mkFreshVar("undef", val.value);
  s.addUndefVar(expr(var));
  return { move(var), move(val.non_poison) };
}


void PoisonValue::print(ostream &os) const {
  UNREACHABLE();
}

StateValue PoisonValue::toSMT(State &s) const {
  return getType().getDummyValue(false);
}


void VoidValue::print(ostream &os) const {
  UNREACHABLE();
}

StateValue VoidValue::toSMT(State &s) const {
  return { false, false };
}


void NullPointerValue::print(ostream &os) const {
  UNREACHABLE();
}

StateValue NullPointerValue::toSMT(State &s) const {
  auto nullp = Pointer::mkNullPointer(s.getMemory());
  return { nullp.release(), true };
}


void GlobalVariable::print(ostream &os) const {
  os << getName() << " = " << (isconst ? "constant " : "global ") << allocsize
     << " bytes, align " << align;
}

static expr get_global(State &s, const string &name, const expr &size,
                       unsigned align, bool isconst, unsigned &bid) {
  expr ptr;
  bool allocated;
  auto blkkind = isconst ? Memory::CONSTGLOBAL : Memory::GLOBAL;

  if (s.hasGlobalVarBid(name, bid, allocated)) {
    if (!allocated) {
      // Use the same block id that is used by src
      assert(!s.isSource());
      ptr = s.getMemory().alloc(size, align, blkkind, true, true, bid).first;
      s.markGlobalAsAllocated(name);
    } else {
      ptr = Pointer(s.getMemory(), bid, false).release();
    }
  } else {
    ptr = s.getMemory().alloc(size, align, blkkind, true, true, nullopt,
                              &bid).first;
    s.addGlobalVarBid(name, bid);
  }
  return ptr;
}

StateValue GlobalVariable::toSMT(State &s) const {
  unsigned bid;
  expr size = expr::mkUInt(allocsize, bits_size_t);
  return { get_global(s, getName(), size, align, isconst, bid), true };
}


static string agg_str(vector<Value*> &vals) {
  string r = "{ ";
  bool first = true;
  for (auto val : vals) {
    if (!first)
      r += ", ";
    r += val->getName();
    first = false;
  }
  return r + " }";
}

AggregateValue::AggregateValue(Type &type, vector<Value*> &&vals)
  : Value(type, agg_str(vals)), vals(move(vals)) {}

StateValue AggregateValue::toSMT(State &s) const {
  vector<StateValue> state_vals;
  for (auto val : vals) {
    state_vals.emplace_back(val->toSMT(s));
  }
  return getType().getAsAggregateType()->aggregateVals(state_vals);
}

expr AggregateValue::getTypeConstraints() const {
  expr r = Value::getTypeConstraints();
  vector<Type*> types;
  for (auto val : vals) {
    types.emplace_back(&val->getType());
    if (dynamic_cast<const Instr*>(val))
      // Instr's type constraints are already generated by BasicBlock's
      // getTypeConstraints()
      continue;
    r &= val->getTypeConstraints();
  }
  return r && getType().enforceAggregateType(&types);
}

void AggregateValue::print(std::ostream &os) const {
  UNREACHABLE();
}


static string attr_str(const ParamAttrs &attr) {
  stringstream ss;
  ss << attr;
  return ss.str();
}

Input::Input(Type &type, string &&name, ParamAttrs &&attributes)
  : Value(type, attr_str(attributes) + name), smt_name(move(name)),
    attrs(move(attributes)) {}

void Input::copySMTName(const Input &other) {
  smt_name = other.smt_name;
}

void Input::print(ostream &os) const {
  UNREACHABLE();
}

StateValue Input::toSMT(State &s) const {
  // 00: normal, 01: undef, else: poison
  expr type = getTyVar();

  bool has_byval = hasAttribute(ParamAttrs::ByVal);
  bool has_deref = hasAttribute(ParamAttrs::Dereferenceable);
  bool has_nonnull = hasAttribute(ParamAttrs::NonNull);
  bool has_noundef = hasAttribute(ParamAttrs::NoUndef);

  expr val;
  if (has_byval) {
    unsigned bid;
    expr size = expr::mkUInt(attrs.blockSize, bits_size_t);
    val = get_global(s, getName(), size, attrs.align, false, bid);
    s.getMemory().markByVal(bid);
  } else {
    val = getType().mkInput(s, smt_name.c_str(), attrs);
  }

  bool never_undef = config::disable_undef_input || has_byval || has_deref ||
                     has_noundef;

  if (!never_undef) {
    auto [undef, vars] = getType().mkUndefInput(s, attrs);
    for (auto &v : vars) {
      s.addUndefVar(move(v));
    }
    val = expr::mkIf(type.extract(0, 0) == 0, val, undef);
  }

  if (has_deref) {
    Pointer p(s.getMemory(), val);
    s.addAxiom(p.isDereferenceable(attrs.derefBytes, bits_byte/8, false));
  }

  expr poison = getType().getDummyValue(false).non_poison;
  expr non_poison = getType().getDummyValue(true).non_poison;
  bool never_poison = config::disable_poison_input || has_byval || has_deref ||
                      has_nonnull || has_noundef;

  if (never_undef) {
    s.addAxiom(never_poison ? type == 0 : type.extract(0, 0) == 0);
  } else if (never_poison) {
    s.addAxiom(type.extract(1, 1) == 0);
  }

  // TODO: element-wise poison/undef control
  return { move(val),
             never_poison
             ? move(non_poison)
             : expr::mkIf(type.extract(1, 1) == 0, non_poison, poison) };
}

expr Input::getTyVar() const {
  string tyname = "ty_" + smt_name;
  return expr::mkVar(tyname.c_str(), 2);
}

}
