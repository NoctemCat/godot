/**************************************************************************/
/*  epoch_owner.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/error/error_macros.h"
#include "core/os/condition_variable.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/os/rw_lock.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/typedefs.h"

#include <atomic>
#include <utility>

template <int Tag>
class EpochOwner {
	// Multiple writers or one exclusive reader.
	template <typename T>
	class _WARN_UNUSED_ PushVector {
		SafeNumeric<uint32_t> count{ 0 };
		SafeNumeric<uint32_t> capacity{ 0 };
		SafeFlag allocating{ false };
		RWLock rwlock;
		T *data = nullptr;

	public:
		_FORCE_INLINE_ T *ptr() _LIFETIME_BOUND_ { return data; }
		_FORCE_INLINE_ const T *ptr() const _LIFETIME_BOUND_ { return data; }
		_FORCE_INLINE_ uint32_t size() const { return count.get(); }

		_FORCE_INLINE_ Span<T> span() const _LIFETIME_BOUND_ { return Span(data, count.get()); }
		_FORCE_INLINE_ operator Span<T>() const _LIFETIME_BOUND_ { return span(); }

		void push(T p_elem) {
			uint32_t elem_idx = count.postadd(1);
			while (unlikely(allocating.is_set() || elem_idx >= capacity.get())) {
				rwlock.write_lock();
				if (allocating.set_if_clear()) {
					reserve(elem_idx + 1);
					allocating.clear();
				}
				rwlock.write_unlock();
			}
			rwlock.read_lock();
			memnew_placement(&data[elem_idx], T(std::move(p_elem)));
			rwlock.read_unlock();
		}

		void reserve(uint32_t p_size) {
			uint32_t old_capacity = capacity.get();
			if (p_size > old_capacity) {
				uint32_t new_capacity = old_capacity > 0 ? old_capacity * 2 : 8;
				T *new_data = memnew_arr(T, new_capacity);
				if (data) {
					copy_arr_placement(new_data, data, old_capacity);
					memdelete_arr(data);
				}
				data = new_data;
				CRASH_COND_MSG(!data, "Out of memory");
				capacity.set(new_capacity);
			} else {
				// Do nothing.
			}
		}

		void clear() {
			count.set(0);
		}
		void reset() {
			clear();
			if (data) {
				memdelete_arr(data);
				data = nullptr;
				capacity.set(0);
			}
		}
		PushVector() = default;
		~PushVector() {
			if (data) {
				reset();
			}
		}
	};

	static constexpr uint32_t EPOCH_COUNT = 3;
	static constexpr uint32_t ADVANCING_FREQUENCY = 1000;
	typedef void (*DeletionFn)(void *p_ptr);

	struct RetireData {
		void *ptr = nullptr;
		DeletionFn deletion_fn = nullptr;
	};

	struct EpochData {
		std::atomic_bool entered{ false };
		LocalVector<RetireData> retired;
	};

	alignas(Thread::CACHE_LINE_BYTES) inline static SafeNumeric<uint32_t> global_epoch{ 1 };
	alignas(Thread::CACHE_LINE_BYTES) inline static PushVector<EpochData *> epoch_thread_datas[EPOCH_COUNT] = {};
	alignas(Thread::CACHE_LINE_BYTES) inline static SafeFlag advancing{ false };
	alignas(Thread::CACHE_LINE_BYTES) inline static std::atomic<uint32_t> advancing_attempts{ 0 };

	inline static SafeFlag is_blocked{ false };
	inline static BinaryMutex epoch_block_mutex;
	inline static BinaryMutex wait_mutex;
	inline static ConditionVariable wait_cond_var;
	inline static SafeNumeric<uint32_t> wait_epoch_idx{ 0 };

	inline static thread_local bool thread_is_blocking = false;
	inline static thread_local uint32_t thread_epoch = 0;
	inline static thread_local uint32_t thread_depth = 0;
	inline static thread_local EpochData *thread_data = nullptr;

	static void _clear_epoch_data(uint32_t p_epoch_idx);

	static bool _is_epoch_active(uint32_t p_epoch_idx);

	static uint32_t _get_next_epoch_idx(uint32_t p_epoch, uint32_t &r_advance);

public:
	static void enter();

	static void exit();

	static void retire(void *p_ptr, DeletionFn p_deletion_fn);
	static void try_advance();

	inline static const char *block_owner = nullptr;
	// Uses non-recursive mutex, don't call enter in the same thread after blocking.
	static void block(const char *p_owner);

	static void unblock();

	// Use with care. Can only be called after blocking the epoch owner.
	// template <bool p_clear_inactive_data>
	static void wait_until_inactive();

	static void sync(const char *p_owner);
};

using DefaultEpoch = EpochOwner<0>;