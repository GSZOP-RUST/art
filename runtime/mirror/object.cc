/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <ctime>

#include "object.h"

#include "art_field.h"
#include "art_field-inl.h"
#include "array-inl.h"
#include "class.h"
#include "class-inl.h"
#include "class_linker-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "iftable-inl.h"
#include "monitor.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "sirt_ref.h"
#include "throwable.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

Object* Object::Clone(Thread* self) {
  mirror::Class* c = GetClass();
  DCHECK(!c->IsClassClass());
  // Object::SizeOf gets the right size even if we're an array.
  // Using c->AllocObject() here would be wrong.
  size_t num_bytes = SizeOf();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<mirror::Object> sirt_this(self, this);
  Object* copy = heap->AllocObject(self, c, num_bytes);
  if (UNLIKELY(copy == nullptr)) {
    return nullptr;
  }
  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  byte* src_bytes = reinterpret_cast<byte*>(sirt_this.get());
  byte* dst_bytes = reinterpret_cast<byte*>(copy);
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);
  // Perform write barriers on copied object references.
  c = copy->GetClass();  // Re-read Class in case it moved.
  if (c->IsArrayClass()) {
    if (!c->GetComponentType()->IsPrimitive()) {
      const ObjectArray<Object>* array = copy->AsObjectArray<Object>();
      heap->WriteBarrierArray(copy, 0, array->GetLength());
    }
  } else {
    heap->WriteBarrierEveryFieldOf(copy);
  }
  if (c->IsFinalizable()) {
    SirtRef<mirror::Object> sirt_copy(self, copy);
    heap->AddFinalizerReference(self, copy);
    return sirt_copy.get();
  }
  return copy;
}

int32_t Object::GenerateIdentityHashCode() {
  static AtomicInteger seed(987654321 + std::time(nullptr));
  int32_t expected_value, new_value;
  do {
    expected_value = static_cast<uint32_t>(seed.load());
    new_value = expected_value * 1103515245 + 12345;
  } while ((expected_value & LockWord::kHashMask) == 0 ||
      !seed.compare_and_swap(expected_value, new_value));
  return expected_value & LockWord::kHashMask;
}

int32_t Object::IdentityHashCode() const {
  while (true) {
    LockWord lw = GetLockWord();
    switch (lw.GetState()) {
      case LockWord::kUnlocked: {
        // Try to compare and swap in a new hash, if we succeed we will return the hash on the next
        // loop iteration.
        LockWord hash_word(LockWord::FromHashCode(GenerateIdentityHashCode()));
        DCHECK_EQ(hash_word.GetState(), LockWord::kHashCode);
        if (const_cast<Object*>(this)->CasLockWord(lw, hash_word)) {
          return hash_word.GetHashCode();
        }
        break;
      }
      case LockWord::kThinLocked: {
        // Inflate the thin lock to a monitor and stick the hash code inside of the monitor.
        Thread* self = Thread::Current();
        Monitor::InflateThinLocked(self, const_cast<Object*>(this), lw, GenerateIdentityHashCode());
        break;
      }
      case LockWord::kFatLocked: {
        // Already inflated, return the has stored in the monitor.
        Monitor* monitor = lw.FatLockMonitor();
        DCHECK(monitor != nullptr);
        return monitor->GetHashCode();
      }
      case LockWord::kHashCode: {
        return lw.GetHashCode();
      }
    }
  }
  LOG(FATAL) << "Unreachable";
  return 0;
}

void Object::CheckFieldAssignmentImpl(MemberOffset field_offset, const Object* new_value) {
  const Class* c = GetClass();
  if (Runtime::Current()->GetClassLinker() == NULL ||
      !Runtime::Current()->GetHeap()->IsObjectValidationEnabled() ||
      !c->IsResolved()) {
    return;
  }
  for (const Class* cur = c; cur != NULL; cur = cur->GetSuperClass()) {
    ObjectArray<ArtField>* fields = cur->GetIFields();
    if (fields != NULL) {
      size_t num_ref_ifields = cur->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_ref_ifields; ++i) {
        ArtField* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  if (c->IsArrayClass()) {
    // Bounds and assign-ability done in the array setter.
    return;
  }
  if (IsClass()) {
    ObjectArray<ArtField>* fields = AsClass()->GetSFields();
    if (fields != NULL) {
      size_t num_ref_sfields = AsClass()->NumReferenceStaticFields();
      for (size_t i = 0; i < num_ref_sfields; ++i) {
        ArtField* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  LOG(FATAL) << "Failed to find field for assignment to " << reinterpret_cast<void*>(this)
      << " of type " << PrettyDescriptor(c) << " at offset " << field_offset;
}

}  // namespace mirror
}  // namespace art
