/*
 * Copyright (c) 2019, 2020, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */


#include "precompiled.hpp"


#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahRootVerifier.hpp"
#include "gc/shenandoah/shenandoahStringDedup.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shared/oopStorage.inline.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"

ShenandoahGCStateResetter::ShenandoahGCStateResetter() :
  _gc_state(ShenandoahHeap::heap()->gc_state()),
  _concurrent_weak_root_in_progress(ShenandoahHeap::heap()->is_concurrent_weak_root_in_progress()) {
}

ShenandoahGCStateResetter::~ShenandoahGCStateResetter() {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  heap->_gc_state.set(_gc_state);
  assert(heap->gc_state() == _gc_state, "Should be restored");
  heap->set_concurrent_weak_root_in_progress(_concurrent_weak_root_in_progress);
}

// Check for overflow of number of root types.
STATIC_ASSERT((static_cast<uint>(ShenandoahRootVerifier::AllRoots) + 1) > static_cast<uint>(ShenandoahRootVerifier::AllRoots));

ShenandoahRootVerifier::ShenandoahRootVerifier(RootTypes types) : _types(types) {
  Threads::change_thread_claim_token();
}

void ShenandoahRootVerifier::excludes(RootTypes types) {
  _types = static_cast<ShenandoahRootVerifier::RootTypes>(static_cast<uint>(_types) & (~static_cast<uint>(types)));
}

bool ShenandoahRootVerifier::verify(RootTypes type) const {
  return (_types & type) == type;
}

ShenandoahRootVerifier::RootTypes ShenandoahRootVerifier::combine(RootTypes t1, RootTypes t2) {
  return static_cast<ShenandoahRootVerifier::RootTypes>(static_cast<uint>(t1) | static_cast<uint>(t2));
}

void ShenandoahRootVerifier::oops_do(OopClosure* oops) {
  ShenandoahGCStateResetter resetter;

  CodeBlobToOopClosure blobs(oops, !CodeBlobToOopClosure::FixRelocations);
  if (verify(CodeRoots)) {
    shenandoah_assert_locked_or_safepoint(CodeCache_lock);
    CodeCache::blobs_do(&blobs);
  }

  if (verify(CLDGRoots)) {
    shenandoah_assert_locked_or_safepoint(ClassLoaderDataGraph_lock);
    CLDToOopClosure clds(oops, ClassLoaderData::_claim_none);
    ClassLoaderDataGraph::cld_do(&clds);
  }

  if (verify(SerialRoots)) {
    shenandoah_assert_safepoint();
  }

  if (verify(JNIHandleRoots)) {
    shenandoah_assert_safepoint();
    JNIHandles::oops_do(oops);
    Universe::vm_global()->oops_do(oops);
  }

  if (verify(WeakRoots)) {
    shenandoah_assert_safepoint();
    AlwaysTrueClosure always_true;
    WeakProcessor::weak_oops_do(&always_true, oops);
  } else if (verify(SerialWeakRoots)) {
    shenandoah_assert_safepoint();
    serial_weak_roots_do(oops);
  } else if (verify(ConcurrentWeakRoots)) {
    concurrent_weak_roots_do(oops);
  }

  if (ShenandoahStringDedup::is_enabled() && verify(StringDedupRoots)) {
    shenandoah_assert_safepoint();
    ShenandoahStringDedup::oops_do_slow(oops);
  }

  if (verify(ThreadRoots)) {
    shenandoah_assert_safepoint();
    // Do thread roots the last. This allows verification code to find
    // any broken objects from those special roots first, not the accidental
    // dangling reference from the thread root.
    Threads::possibly_parallel_oops_do(false, oops, &blobs);
  }
}

void ShenandoahRootVerifier::roots_do(OopClosure* oops) {
  ShenandoahGCStateResetter resetter;
  shenandoah_assert_safepoint();

  CodeBlobToOopClosure blobs(oops, !CodeBlobToOopClosure::FixRelocations);
  CodeCache::blobs_do(&blobs);

  CLDToOopClosure clds(oops, ClassLoaderData::_claim_none);
  ClassLoaderDataGraph::cld_do(&clds);

  JNIHandles::oops_do(oops);
  Universe::vm_global()->oops_do(oops);

  AlwaysTrueClosure always_true;
  WeakProcessor::weak_oops_do(&always_true, oops);

  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::oops_do_slow(oops);
  }

  // Do thread roots the last. This allows verification code to find
  // any broken objects from those special roots first, not the accidental
  // dangling reference from the thread root.
  Threads::possibly_parallel_oops_do(true, oops, &blobs);
}

void ShenandoahRootVerifier::strong_roots_do(OopClosure* oops) {
  ShenandoahGCStateResetter resetter;
  shenandoah_assert_safepoint();

  CodeBlobToOopClosure blobs(oops, !CodeBlobToOopClosure::FixRelocations);

  CLDToOopClosure clds(oops, ClassLoaderData::_claim_none);
  ClassLoaderDataGraph::roots_cld_do(&clds, NULL);

  JNIHandles::oops_do(oops);
  Universe::vm_global()->oops_do(oops);

  // Do thread roots the last. This allows verification code to find
  // any broken objects from those special roots first, not the accidental
  // dangling reference from the thread root.
  Threads::possibly_parallel_oops_do(true, oops, &blobs);
}

void ShenandoahRootVerifier::serial_weak_roots_do(OopClosure* cl) {
  WeakProcessorPhases::Iterator itr = WeakProcessorPhases::serial_iterator();
  AlwaysTrueClosure always_true;
  for ( ; !itr.is_end(); ++itr) {
    WeakProcessorPhases::processor(*itr)(&always_true, cl);
  }
}

void ShenandoahRootVerifier::concurrent_weak_roots_do(OopClosure* cl) {
  for (OopStorageSet::Iterator it = OopStorageSet::weak_iterator(); !it.is_end(); ++it) {
    OopStorage* storage = *it;
    storage->oops_do<OopClosure>(cl);
  }
}
