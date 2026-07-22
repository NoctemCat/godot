/**************************************************************************/
/*  epoch_owner.cpp                                                       */
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

#include "epoch_owner.h"

#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include <atomic>

template <int Tag>
void EpochOwner<Tag>::_clear_epoch_data(uint32_t p_epoch_idx) {
	for (EpochData *epoch_thread_data : epoch_thread_datas[p_epoch_idx].span()) {
		for (const RetireData &data : epoch_thread_data->retired) {
			if (data.deletion_fn) {
				data.deletion_fn(data.ptr);
			}
		}
		memdelete(epoch_thread_data);
		// if (epoch_thread_data != thread_data) {
		// } else {
		// 	epoch_thread_data->retired.clear();
		// }
	}
	epoch_thread_datas[p_epoch_idx].clear();
}

template <int Tag>
bool EpochOwner<Tag>::_is_epoch_active(uint32_t p_epoch_idx) {
	for (EpochData *data : epoch_thread_datas[p_epoch_idx].span()) {
		if (data->entered.load(std::memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

template <int Tag>
uint32_t EpochOwner<Tag>::_get_next_epoch_idx(uint32_t p_epoch, uint32_t &r_advance) {
	r_advance = 1;
	uint32_t epoch_idx = p_epoch % EPOCH_COUNT;
	uint32_t next_epoch_idx = (p_epoch + r_advance) % EPOCH_COUNT;

	// Prevent any possible issues with overflow.
	if (epoch_idx == next_epoch_idx) {
		r_advance = 2;
		next_epoch_idx = (p_epoch + r_advance) % EPOCH_COUNT;
	}
	return next_epoch_idx;
}

template <int Tag>
void EpochOwner<Tag>::enter() {
	if (thread_depth == 0) {
		if (unlikely(is_blocked.is_set())) {
			if (thread_is_blocking) {
				CRASH_NOW_MSG("Enter in block.");
			}
			MutexLock lock(epoch_block_mutex);
		}
		uint32_t epoch = global_epoch.get();
		if (thread_epoch != epoch) {
			thread_epoch = epoch;
			thread_data = memnew(EpochData);
			epoch_thread_datas[thread_epoch % EPOCH_COUNT].push(thread_data);
		}
		thread_data->entered.store(true, std::memory_order_release);
	}
	thread_depth++;
}

template <int Tag>
void EpochOwner<Tag>::exit() {
	thread_depth--;
	if (thread_depth != 0) {
		return;
	}
	if (likely(!is_blocked.is_set())) {
		thread_data->entered.store(false, std::memory_order_relaxed);
		// Don't load wait_epoch_idx if you don't need to.
	} else {
		// Only need to notify if we are waiting on the current epoch.
		if (likely(thread_epoch % EPOCH_COUNT != wait_epoch_idx.get())) {
			thread_data->entered.store(false, std::memory_order_relaxed);
		} else {
			{
				MutexLock lock(wait_mutex);
				thread_data->entered.store(false, std::memory_order_relaxed);
			}
			wait_cond_var.notify_all();
		}
	}
}

template <int Tag>
void EpochOwner<Tag>::retire(void *p_ptr, DeletionFn p_deletion_fn) {
	thread_data->retired.push_back(RetireData{ p_ptr, p_deletion_fn });
}

template <int Tag>
void EpochOwner<Tag>::try_advance() {
	uint32_t attempts = advancing_attempts.fetch_add(1, std::memory_order_relaxed);
	if (attempts < ADVANCING_FREQUENCY) {
		return;
	}
	if (unlikely(is_blocked.is_set())) {
		return;
	}
	if (!advancing.set_if_clear()) {
		return;
	}

	uint32_t advance = 1;
	uint32_t next_epoch_idx = _get_next_epoch_idx(global_epoch.get(), advance);

	if (!_is_epoch_active(next_epoch_idx)) {
		// Advance if the next epoch is empty.
		_clear_epoch_data(next_epoch_idx);
		global_epoch.add(advance);
	}

	if (likely(!is_blocked.is_set())) {
		advancing.clear();
	} else {
		{
			MutexLock lock(wait_mutex);
			advancing.clear();
		}
		wait_cond_var.notify_all();
	}
	advancing_attempts.store(0, std::memory_order_relaxed);
}

// Uses non-recursive mutex, don't call enter in the same thread after blocking.
template <int Tag>
void EpochOwner<Tag>::block(const char *p_owner) {
	print_verbose("Epoch " + (String)p_owner + " blocks.");
	epoch_block_mutex.lock();
	print_verbose("Epoch " + (String)p_owner + " after mutex.");
	block_owner = p_owner;
	is_blocked.set();
	thread_is_blocking = true;
}

template <int Tag>
void EpochOwner<Tag>::unblock() {
	print_verbose("Epoch " + (String)block_owner + " unblocks.");
	thread_is_blocking = false;
	block_owner = nullptr;
	is_blocked.clear();
	epoch_block_mutex.unlock();
}

// Use with care. Can only be called after blocking the epoch owner.
template <int Tag>
void EpochOwner<Tag>::wait_until_inactive() {
	DEV_ASSERT(is_blocked.is_set());

	{
		MutexLock lock(wait_mutex);
		print_verbose("Epoch " + (String)block_owner + " begin wait.");
		wait_cond_var.wait(lock, []() { return !advancing.is_set(); });
		print_verbose("Epoch " + (String)block_owner + " after advancing.");
	}

	uint32_t epoch = global_epoch.get();
	for (uint32_t i = 0; i < EPOCH_COUNT; i++) {
		uint32_t advance = 1;
		uint32_t next_epoch_idx = _get_next_epoch_idx(epoch, advance);
		epoch += advance;
		{
			MutexLock lock(wait_mutex);
			wait_epoch_idx.set(next_epoch_idx);
			print_verbose("Epoch " + (String)block_owner + " wait for " + itos(epoch) + ".");
			wait_cond_var.wait(lock, [next_epoch_idx]() {
				return !_is_epoch_active(next_epoch_idx);
			});
		}

		// if constexpr (p_clear_inactive_data)
		{
			_clear_epoch_data(next_epoch_idx);
		}
		print_verbose("Epoch " + (String)block_owner + " " + itos(i) + " finished.");
	}

	// if constexpr (p_clear_inactive_data)
	{
		// The data from the current epoch is also cleared,
		// advance the epoch to get the new data on the next enter.
		uint32_t advance = 1;
		_get_next_epoch_idx(global_epoch.get(), advance);
		global_epoch.add(advance);
	}
	print_verbose("Epoch " + (String)block_owner + " finished wait.");
}

template <int Tag>
void EpochOwner<Tag>::sync(const char *p_owner) {
	block(p_owner);
	wait_until_inactive();
	unblock();
}

// EpochOwner<
template class EpochOwner<0>;
template class EpochOwner<1>;
template class EpochOwner<2>;