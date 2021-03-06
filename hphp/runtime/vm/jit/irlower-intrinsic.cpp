/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/irlower-internal.h"

#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/ext/hh/ext_hh.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/memo-cache.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/srckey.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/bc-marker.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/write-lease.h"

#include "hphp/util/trace.h"

namespace HPHP { namespace jit { namespace irlower {

TRACE_SET_MOD(irlower);

///////////////////////////////////////////////////////////////////////////////

void cgNop(IRLS&, const IRInstruction*) {}
void cgDefConst(IRLS&, const IRInstruction*) {}
void cgEndGuards(IRLS&, const IRInstruction*) {}
void cgExitPlaceholder(IRLS&, const IRInstruction*) {}

///////////////////////////////////////////////////////////////////////////////

void cgFuncGuard(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<FuncGuard>();
  vmain(env) << funcguard{extra->func, extra->prologueAddrPtr};
}

void cgDefFP(IRLS&, const IRInstruction*) {}

void cgDefSP(IRLS& env, const IRInstruction* inst) {
  auto const sp = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  if (inst->marker().resumeMode() != ResumeMode::None) {
    v << defvmsp{sp};
    return;
  }

  auto const fp = srcLoc(env, inst, 0).reg();
  v << lea{fp[-cellsToBytes(inst->extra<DefSP>()->offset.offset)], sp};
}

void cgEagerSyncVMRegs(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<EagerSyncVMRegs>();
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const sp = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);

  auto const sync_sp = v.makeReg();
  v << lea{sp[cellsToBytes(extra->offset.offset)], sync_sp};
  emitEagerSyncPoint(v, inst->marker().fixupSk().pc(), rvmtl(), fp, sync_sp);
}

void cgMov(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0);
  auto const src = srcLoc(env, inst, 0);
  always_assert(inst->src(0)->numWords() == inst->dst(0)->numWords());
  copyTV(vmain(env), src, dst, inst->dst()->type());
}

void cgUnreachable(IRLS& env, const IRInstruction* inst) {
  auto reason = inst->extra<AssertReason>()->reason;
  vmain(env) << trap{reason};
}

void cgEndBlock(IRLS& env, const IRInstruction* inst) {
  auto reason = inst->extra<AssertReason>()->reason;
  vmain(env) << trap{reason};
}

///////////////////////////////////////////////////////////////////////////////

void cgInterpOne(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<InterpOne>();
  auto const sp = srcLoc(env, inst, 0).reg();

  // Did you forget to specify ControlFlowInfo?
  assertx(!instrIsControlFlow(extra->opcode));
  auto const helper = interpOneEntryPoints[size_t(extra->opcode)];
  auto const args = argGroup(env, inst)
    .ssa(1)
    .addr(sp, cellsToBytes(extra->spOffset.offset))
    .imm(extra->bcOff);

  // Call the interpOne##Op() routine, which syncs VM regs manually.
  cgCallHelper(vmain(env), env, CallSpec::direct(helper),
               kVoidDest, SyncOptions::None, args);
}

void cgInterpOneCF(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<InterpOneCF>();
  auto const sp = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const sync_sp = v.makeReg();
  v << lea{sp[cellsToBytes(extra->spOffset.offset)], sync_sp};
  v << syncvmsp{sync_sp};

  assertx(tc::ustubs().interpOneCFHelpers.count(extra->opcode));

  // We pass the Offset in the third argument register.
  v << ldimml{extra->bcOff, rarg(2)};
  v << jmpi{tc::ustubs().interpOneCFHelpers.at(extra->opcode),
            interp_one_cf_regs()};
}

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(GetTime);
IMPL_OPCODE_CALL(GetTimeNs);

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(PrintBool)
IMPL_OPCODE_CALL(PrintInt)
IMPL_OPCODE_CALL(PrintStr)

///////////////////////////////////////////////////////////////////////////////

namespace {

void getMemoKeyImpl(IRLS& env, const IRInstruction* inst, bool sync) {
  auto const s = inst->src(0);

  auto args = argGroup(env, inst);
  if (s->isA(TArrLike) || s->isA(TObj) || s->isA(TStr) || s->isA(TDbl)) {
    args.ssa(0, s->isA(TDbl));
  } else {
    args.typedValue(0);
  }

  auto const target = [&]{
    if (s->isA(TArrLike)) return CallSpec::direct(serialize_memoize_param_arr);
    if (s->isA(TStr))     return CallSpec::direct(serialize_memoize_param_str);
    if (s->isA(TDbl))     return CallSpec::direct(serialize_memoize_param_dbl);
    if (s->isA(TObj)) {
      auto const ty = s->type();
      if (ty.clsSpec().cls() && ty.clsSpec().cls()->isCollectionClass()) {
        return CallSpec::direct(serialize_memoize_param_col);
      }
      return CallSpec::direct(serialize_memoize_param_obj);
    }
    return CallSpec::direct(HHVM_FN(serialize_memoize_param));
  }();

  cgCallHelper(
    vmain(env),
    env,
    target,
    callDestTV(env, inst),
    sync ? SyncOptions::Sync :  SyncOptions::None,
    args
  );
}

}

void cgGetMemoKey(IRLS& env, const IRInstruction* inst) {
  getMemoKeyImpl(env, inst, true);
}

void cgGetMemoKeyScalar(IRLS& env, const IRInstruction* inst) {
  getMemoKeyImpl(env, inst, false);
}

///////////////////////////////////////////////////////////////////////////////

static void memoSetDecRefImpl(Cell newVal, Cell* oldVal) {
  cellSet(newVal, *oldVal);
}

void cgMemoGetStaticValue(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const f = inst->extra<MemoGetStaticValue>()->func;
  auto const cache = rds::bindStaticMemoValue(f);
  auto const sf = checkRDSHandleInitialized(v, cache.handle());
  fwdJcc(v, env, CC_NE, sf, inst->taken());
  loadTV(v, inst->dst(), dstLoc(env, inst, 0), rvmtl()[cache.handle()]);
}

void cgMemoSetStaticValue(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const f = inst->extra<MemoSetStaticValue>()->func;
  auto const val = inst->src(0);
  auto const valLoc = srcLoc(env, inst, 0);
  auto const cache = rds::bindStaticMemoValue(f);

  // Store the value (overwriting any previous value)

  auto const memoTy = typeFromRAT(f->repoReturnType(), f->cls()) & TInitCell;
  if (!memoTy.maybe(TCounted)) {
    assertx(!val->type().maybe(TCounted));
    storeTV(v, rvmtl()[cache.handle()], valLoc, val);
    markRDSHandleInitialized(v, cache.handle());
    return;
  }

  auto const sf = checkRDSHandleInitialized(v, cache.handle());
  unlikelyIfThenElse(
    v, vcold(env), CC_E, sf,
    [&](Vout& v) {
      cgCallHelper(
        v,
        env,
        CallSpec::direct(memoSetDecRefImpl),
        kVoidDest,
        SyncOptions::Sync,
        argGroup(env, inst)
          .typedValue(0)
          .addr(rvmtl(), safe_cast<int32_t>(cache.handle()))
      );
    },
    [&](Vout& v) {
      emitIncRefWork(v, valLoc, val->type(), TRAP_REASON);
      storeTV(v, rvmtl()[cache.handle()], valLoc, val);
      markRDSHandleInitialized(v, cache.handle());
    }
  );
}

void cgMemoGetStaticCache(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<MemoGetStaticCache>();
  // We need some keys, or this would be GetStaticValue.
  assertx(extra->keys.count > 0);

  // If the RDS entry isn't initialized, there can't be a value.
  auto const cache = rds::bindStaticMemoCache(extra->func);
  auto const sf = checkRDSHandleInitialized(v, cache.handle());
  fwdJcc(v, env, CC_NE, sf, inst->taken());

  auto const cachePtr = v.makeReg();
  v << load{rvmtl()[cache.handle()], cachePtr};

  // Lookup the proper getter function and call it with the pointer to the
  // cache. The pointer to the cache may be null, but the getter function can
  // handle that.
  auto const valPtr = v.makeReg();
  if (auto const getter =
      memoCacheGetForKeyTypes(extra->types, extra->keys.count)) {
    auto const args = argGroup(env, inst)
      .reg(cachePtr)
      .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1));
    cgCallHelper(
      v,
      env,
      CallSpec::direct(getter),
      callDest(valPtr),
      SyncOptions::None,
      args
    );
  } else {
    auto const args = argGroup(env, inst)
      .reg(cachePtr)
      .imm(GenericMemoId{extra->func->getFuncId(), extra->keys.count}.asParam())
      .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1));
    cgCallHelper(
      v,
      env,
      CallSpec::direct(memoCacheGetGeneric),
      callDest(valPtr),
      SyncOptions::None,
      args
    );
  }

  // If the returned pointer isn't null, load the value out of it.
  auto const sf2 = v.makeReg();
  v << testq{valPtr, valPtr, sf2};
  fwdJcc(v, env, CC_Z, sf2, inst->taken());
  loadTV(v, inst->dst(), dstLoc(env, inst, 0), *valPtr);
}

void cgMemoSetStaticCache(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<MemoSetStaticCache>();
  assertx(extra->keys.count > 0);

  // If the RDS entry isn't initialized, mark it as initialized and store a null
  // pointer in it. The setter will allocate the cache and update the pointer.
  auto const cache = rds::bindStaticMemoCache(extra->func);
  auto const sf = checkRDSHandleInitialized(v, cache.handle());
  ifThen(
    v, CC_NE, sf,
    [&](Vout& v) {
      v << storeqi{0, rvmtl()[cache.handle()]};
      markRDSHandleInitialized(v, cache.handle());
    }
  );

  // Lookup the setter and call it with the address of the cache pointer. The
  // setter will create the cache as needed and update the pointer.
  if (auto const setter =
      memoCacheSetForKeyTypes(extra->types, extra->keys.count)) {
    auto const args = argGroup(env, inst)
      .addr(rvmtl(), safe_cast<int32_t>(cache.handle()))
      .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1))
      .typedValue(1);
    cgCallHelper(
      v,
      env,
      CallSpec::direct(setter),
      kVoidDest,
      SyncOptions::Sync,
      args
    );
  } else {
    auto const args = argGroup(env, inst)
      .addr(rvmtl(), safe_cast<int32_t>(cache.handle()))
      .imm(GenericMemoId{extra->func->getFuncId(), extra->keys.count}.asParam())
      .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1))
      .typedValue(1);
    cgCallHelper(
      v,
      env,
      CallSpec::direct(memoCacheSetGeneric),
      kVoidDest,
      SyncOptions::Sync,
      args
    );
  }
}

///////////////////////////////////////////////////////////////////////////////

namespace {

int32_t offsetToMemoSlot(Slot slot, const Class* cls) {
  if (auto const ndi = cls->getNativeDataInfo()) {
    return -safe_cast<int32_t>(
      alignTypedValue(ndi->sz) + sizeof(MemoSlot) * (slot + 1)
    );
  }
  return -safe_cast<int32_t>(sizeof(MemoSlot) * (slot + 1));
}

}

void cgMemoGetInstanceValue(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const extra = inst->extra<MemoGetInstanceValue>();
  auto const obj = srcLoc(env, inst, 0).reg();
  auto const valPtr = obj[offsetToMemoSlot(extra->slot, extra->func->cls())];
  auto const sf = v.makeReg();
  // Uninit means the cache isn't initialized
  emitCmpTVType(v, sf, KindOfUninit, valPtr + TVOFF(m_type));
  fwdJcc(v, env, CC_E, sf, inst->taken());
  loadTV(v, inst->dst(), dstLoc(env, inst, 0), valPtr);
}

void cgMemoSetInstanceValue(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const extra = inst->extra<MemoSetInstanceValue>();
  auto const val = inst->src(1);
  auto const valLoc = srcLoc(env, inst, 1);
  auto const obj = srcLoc(env, inst, 0).reg();
  auto const oldValPtr = obj[offsetToMemoSlot(extra->slot, extra->func->cls())];

  // Mark the object as having valid memo data so that it gets cleaned up upon
  // destruction.
  static_assert(ObjectData::sizeofAttrs() == 2, "");
  v << orwim{
    ObjectData::UsedMemoCache,
    obj[ObjectData::offsetofAttrs()],
    v.makeReg()
  };

  // Store it (overwriting any previous value).

  auto const memoTy =
    typeFromRAT(extra->func->repoReturnType(), extra->func->cls()) & TInitCell;
  if (!memoTy.maybe(TCounted)) {
    assertx(!val->type().maybe(TCounted));
    storeTV(v, oldValPtr, valLoc, val);
    return;
  }

  auto const sf = v.makeReg();
  emitCmpTVType(v, sf, KindOfUninit, oldValPtr + TVOFF(m_type));
  unlikelyIfThenElse(
    v, vcold(env), CC_NE, sf,
    [&](Vout& v) {
      auto const dest = v.makeReg();
      v << lea{oldValPtr, dest};
      cgCallHelper(
        v,
        env,
        CallSpec::direct(memoSetDecRefImpl),
        kVoidDest,
        SyncOptions::Sync,
        argGroup(env, inst).typedValue(1).reg(dest)
      );
    },
    [&](Vout& v) {
      emitIncRefWork(v, valLoc, val->type(), TRAP_REASON);
      storeTV(v, oldValPtr, valLoc, val);
    }
  );
}

void cgMemoGetInstanceCache(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<MemoGetInstanceCache>();
  // Unlike for the static case, we can have zero keys here (because of shared
  // caches).
  auto const obj = srcLoc(env, inst, 1).reg();
  auto const cachePtr =
    obj[offsetToMemoSlot(extra->slot, extra->func->cls()) + TVOFF(m_data)];

  auto const cache = v.makeReg();
  v << load{cachePtr, cache};

  // Short-circuit if the pointer to the cache is null
  auto const sf = v.makeReg();
  v << testq{cache, cache, sf};
  fwdJcc(v, env, CC_Z, sf, inst->taken());

  // Lookup the right getter function and call it with the pointer to the cache.
  auto const valPtr = v.makeReg();
  if (extra->shared) {
    if (extra->keys.count == 0) {
      auto const args = argGroup(env, inst)
        .reg(cache)
        .imm(makeSharedOnlyKey(extra->func->getFuncId()));
      cgCallHelper(
        v,
        env,
        CallSpec::direct(memoCacheGetSharedOnly),
        callDest(valPtr),
        SyncOptions::None,
        args
      );
    } else {
      auto const getter =
        sharedMemoCacheGetForKeyTypes(extra->types, extra->keys.count);
      auto const funcId = extra->func->getFuncId();
      auto const args = argGroup(env, inst)
        .reg(cache)
        .imm(
          getter
            ? funcId
            : GenericMemoId{funcId, extra->keys.count}.asParam()
        )
        .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1));
      cgCallHelper(
        v,
        env,
        getter
          ? CallSpec::direct(getter)
          : CallSpec::direct(memoCacheGetGeneric),
        callDest(valPtr),
        SyncOptions::None,
        args
      );
    }
  } else {
    // A non-shared cache should always have non-zero keys (it would be a memo
    // value otherwise).
    assertx(extra->keys.count > 0);

    auto const getter =
      memoCacheGetForKeyTypes(extra->types, extra->keys.count);

    auto args = argGroup(env, inst).reg(cache);
    if (!getter) {
      args.imm(
        GenericMemoId{extra->func->getFuncId(), extra->keys.count}.asParam()
      );
    }
    args.addr(fp, localOffset(extra->keys.first + extra->keys.count - 1));

    cgCallHelper(
      v,
      env,
      getter
        ? CallSpec::direct(getter)
        : CallSpec::direct(memoCacheGetGeneric),
      callDest(valPtr),
      SyncOptions::None,
      args
    );
  }

  // Check the return pointer, and if it isn't null, load the value out of it.
  auto const sf2 = v.makeReg();
  v << testq{valPtr, valPtr, sf2};
  fwdJcc(v, env, CC_Z, sf2, inst->taken());
  loadTV(v, inst->dst(), dstLoc(env, inst, 0), *valPtr);
}

void cgMemoSetInstanceCache(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<MemoSetInstanceCache>();
  // Unlike the static case, we can have zero keys here (because of shared
  // caches).
  auto const obj = srcLoc(env, inst, 1).reg();
  auto const slotOff = offsetToMemoSlot(extra->slot, extra->func->cls());

  // First mark the object as having valid memo data, so it gets cleaned up upon
  // destruction.
  static_assert(ObjectData::sizeofAttrs() == 2, "");
  v << orwim{
    ObjectData::UsedMemoCache,
    obj[ObjectData::offsetofAttrs()],
    v.makeReg()
  };
  // Also set the type field in the memo slot to indicate this is definitely a
  // cache.
  v << storebi{
    static_cast<data_type_t>(kInvalidDataType),
    obj[slotOff + TVOFF(m_type)]
  };

  // Lookup the right setter and call it with the address of the pointer to the
  // cache. If the pointer is null, the setter will allocate a new cache and
  // update the pointer.
  if (extra->shared) {
    if (extra->keys.count == 0) {
      auto const args = argGroup(env, inst)
        .addr(obj, slotOff + TVOFF(m_data))
        .imm(makeSharedOnlyKey(extra->func->getFuncId()))
        .typedValue(2);
      cgCallHelper(
        v,
        env,
        CallSpec::direct(memoCacheSetSharedOnly),
        kVoidDest,
        SyncOptions::Sync,
        args
      );
    } else {
      auto const setter =
        sharedMemoCacheSetForKeyTypes(extra->types, extra->keys.count);
      auto const funcId = extra->func->getFuncId();
      auto const args = argGroup(env, inst)
        .addr(obj, slotOff + TVOFF(m_data))
        .imm(
          setter
            ? funcId
            : GenericMemoId{funcId, extra->keys.count}.asParam()
        )
        .addr(fp, localOffset(extra->keys.first + extra->keys.count - 1))
        .typedValue(2);
      cgCallHelper(
        v,
        env,
        setter
          ? CallSpec::direct(setter)
          : CallSpec::direct(memoCacheSetGeneric),
        kVoidDest,
        SyncOptions::Sync,
        args
      );
    }
  } else {
    assertx(extra->keys.count > 0);

    auto const setter =
      memoCacheSetForKeyTypes(extra->types, extra->keys.count);

    auto args = argGroup(env, inst).addr(obj, slotOff + TVOFF(m_data));
    if (!setter) {
      args.imm(
        GenericMemoId{extra->func->getFuncId(), extra->keys.count}.asParam()
      );
    }
    args.addr(fp, localOffset(extra->keys.first + extra->keys.count - 1));
    args.typedValue(2);

    cgCallHelper(
      v,
      env,
      setter
        ? CallSpec::direct(setter)
        : CallSpec::direct(memoCacheSetGeneric),
      kVoidDest,
      SyncOptions::Sync,
      args
    );
  }
}

///////////////////////////////////////////////////////////////////////////////

void cgRBTraceEntry(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<RBTraceEntry>();

  auto const args = argGroup(env, inst)
    .imm(extra->type)
    .imm(extra->sk.toAtomicInt());

  cgCallHelper(vmain(env), env, CallSpec::direct(Trace::ringbufferEntryRip),
               kVoidDest, SyncOptions::None, args);
}

void cgRBTraceMsg(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<RBTraceMsg>();
  assertx(extra->msg->isStatic());

  auto const args = argGroup(env, inst)
    .immPtr(extra->msg->data())
    .imm(extra->msg->size())
    .imm(extra->type);

  cgCallHelper(vmain(env), env, CallSpec::direct(Trace::ringbufferMsg),
               kVoidDest, SyncOptions::None, args);
}

///////////////////////////////////////////////////////////////////////////////

void cgIncStat(IRLS& env, const IRInstruction *inst) {
  auto const stat = Stats::StatCounter(inst->src(0)->intVal());
  emitIncStat(vmain(env), stat);
}

void cgIncProfCounter(IRLS& env, const IRInstruction* inst) {
  auto const transID = inst->extra<TransIDData>()->transId;
  auto const counterAddr = profData()->transCounterAddr(transID);
  auto& v = vmain(env);

  if (RuntimeOption::EvalJitPGORacyProfiling) {
    v << decqm{v.cns(counterAddr)[0], v.makeReg()};
  } else {
    v << decqmlock{v.cns(counterAddr)[0], v.makeReg()};
  }
}

void cgCheckCold(IRLS& env, const IRInstruction* inst) {
  auto const transID = inst->extra<CheckCold>()->transId;
  auto const counterAddr = profData()->transCounterAddr(transID);
  auto& v = vmain(env);

  auto const sf = v.makeReg();
  v << decqmlock{v.cns(counterAddr)[0], sf};
  if (RuntimeOption::EvalJitFilterLease) {
    auto filter = v.makeBlock();
    v << jcc{CC_LE, sf, {label(env, inst->next()), filter}};
    v = filter;
    auto const res = v.makeReg();
    cgCallHelper(v, env, CallSpec::direct(couldAcquireOptimizeLease),
                 callDest(res), SyncOptions::None,
                 argGroup(env, inst).immPtr(inst->func()));
    auto const sf2 = v.makeReg();
    v << testb{res, res, sf2};
    v << jcc{CC_NZ, sf2, {label(env, inst->next()), label(env, inst->taken())}};
  } else {
    v << jcc{CC_LE, sf, {label(env, inst->next()), label(env, inst->taken())}};
  }
}

///////////////////////////////////////////////////////////////////////////////

}}}
