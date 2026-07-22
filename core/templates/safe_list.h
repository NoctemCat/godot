/**************************************************************************/
/*  safe_list.h                                                           */
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

#include "core/os/memory.h"
#include "core/templates/epoch_owner.h"
#include "core/typedefs.h"

#include <atomic>
#include <initializer_list>

// Design goals for these classes:
// - Accessing this list with an iterator will never result in a use-after free,
//   even if the element being accessed has been logically removed from the list on
//   another thread.
// - Logical deletion from the list will not result in deallocation at that time,
//   instead the node will be deallocated at a later time when it is safe to do so.
// - No blocking synchronization primitives will be used.

// This is used in very specific areas of the engine where it's critical that these guarantees are held.

template <typename T, typename A = DefaultAllocator, typename EBR = DefaultEpoch>
class SafeList {
	template <typename U>
	static bool is_marked_ptr(U *p_ptr) {
		return (reinterpret_cast<std::uintptr_t>(p_ptr) & 0x1) == 0x1;
	}
	template <typename U>
	static U *get_marked_ptr(U *p_ptr) {
		return reinterpret_cast<U *>(reinterpret_cast<std::uintptr_t>(p_ptr) | 0x1);
	}
	template <typename U>
	static U *get_unmarked_ptr(U *p_ptr) {
		return reinterpret_cast<U *>(reinterpret_cast<std::uintptr_t>(p_ptr) & ~0x1);
	}

	using DeletionFunc = void (*)(T);

	struct SafeListNode {
		std::atomic<SafeListNode *> next{ nullptr };
		DeletionFunc deletion_fn = nullptr;
		T val;
	};

	static_assert(std::atomic<T>::is_always_lock_free);

	SafeListNode *head = nullptr;

public:
	class Iterator {
		friend class SafeList;

		SafeListNode *cursor;

		Iterator(SafeListNode *p_cursor) :
				cursor(p_cursor) {
			EBR::enter();
		}

	public:
		Iterator(const Iterator &p_other) :
				cursor(p_other.cursor) {
			EBR::enter();
		}

		~Iterator() {
			EBR::exit();
		}

	public:
		T &operator*() { return cursor->val; }

		Iterator &operator++() {
			SafeListNode *tmp = nullptr;
			do {
				SafeListNode *tmp = cursor->next.load();
				cursor = get_unmarked_ptr(tmp);
			} while (cursor && is_marked_ptr(tmp));
			return *this;
		}

		// These two operators are mostly useful for comparisons to nullptr.
		bool operator==(const void *p_other) const { return cursor == p_other; }

		bool operator!=(const void *p_other) const { return cursor != p_other; }

		// These two allow easy range-based for loops.
		bool operator==(const Iterator &p_other) const { return cursor == p_other.cursor; }

		bool operator!=(const Iterator &p_other) const { return cursor != p_other.cursor; }
	};

private:
	template <typename ConditionFuncT>
	bool _erase(DeletionFunc p_deletion_fn, ConditionFuncT &&p_search_cond) {
		EBR::enter();

		SafeListNode *prev;
		SafeListNode *node;
		SafeListNode *next;
		do {
			if (!_search_and_unlink(prev, node, next, std::forward<ConditionFuncT>(p_search_cond))) {
				EBR::exit();
				return false;
			}
		} while (!node->next.compare_exchange_strong(
				next, get_marked_ptr(next), std::memory_order_release, std::memory_order_relaxed));

		if (p_deletion_fn) {
			node->deletion_fn = p_deletion_fn;
		}
		if (prev->next.compare_exchange_strong(
					node, next, std::memory_order_release, std::memory_order_relaxed)) {
			retire_node(node);
		} else {
			const SafeListNode *missed_node = next;
			_search_and_unlink(prev, node, next, [&missed_node](SafeListNode *p_node) {
				return p_node == missed_node;
			});
		}

		EBR::exit();
		return true;
	}

	template <typename ConditionFuncT>
	bool _search_and_unlink(
			SafeListNode *&r_prev, SafeListNode *&r_node, SafeListNode *&r_next, ConditionFuncT &&p_search_cond) {
		bool try_again = true;
		while (try_again) {
			try_again = false;

			r_prev = head;
			r_node = head->next.load(std::memory_order_acquire);
			while (r_node != nullptr) {
				SafeListNode *node_next = r_node->next.load(std::memory_order_acquire);
				if (is_marked_ptr(node_next)) {
					if (!r_prev->next.compare_exchange_strong(
								r_node, get_unmarked_ptr(node_next), std::memory_order_release,
								std::memory_order_relaxed)) {
						// Failed to unlink the current node, try again.
						try_again = true;
						break;
					}
					retire_node(r_node);
					r_node = get_unmarked_ptr(node_next);
				} else {
					if (std::forward<ConditionFuncT>(p_search_cond)(r_node)) {
						r_next = node_next;
						return true;
					}
					r_prev = r_node;
					r_node = node_next;
				}
			}
		}
		return false;
	}

	static void retire_node(SafeListNode *p_node) {
		EBR::retire(p_node, [](void *p_void_node) {
			SafeListNode *node = (SafeListNode *)p_void_node;
			if (node->deletion_fn) {
				node->deletion_fn(node->val);
			}
			memdelete_allocator<SafeListNode, A>(node);
		});
	}

public:
	// Calling this will cause an allocation.
	void insert(T p_value, DeletionFunc p_deletion_fn = nullptr) {
		SafeListNode *new_node = memnew_allocator(SafeListNode, A);
		new_node->deletion_fn = p_deletion_fn;
		new_node->val = std::move(p_value);
		SafeListNode *expected_head = nullptr;
		do {
			expected_head = head->next.load(std::memory_order_acquire);
			new_node->next.store(expected_head, std::memory_order_release);
		} while (!head->next.compare_exchange_weak(
				expected_head, new_node, std::memory_order_release, std::memory_order_relaxed));
	}

	bool pop(T &r_value) {
		EBR::enter();
		SafeListNode *old_head = nullptr;
		do {
			old_head = head->next.load(std::memory_order_acquire);
			if (old_head == nullptr) {
				EBR::exit();
				return false;
			}
		} while (!head->next.compare_exchange_weak(old_head, old_head->next.load(std::memory_order_acquire), std::memory_order_release, std::memory_order_relaxed));

		r_value = old_head->val;
		retire_node(old_head);
		EBR::exit();
		return true;
	}

	Iterator find(const T &p_value) {
		for (Iterator it = begin(); it.cursor != nullptr; ++it) {
			if (*it == p_value) {
				return it;
			}
		}
		return end();
	}

	bool erase(const T &p_value, DeletionFunc p_deletion_fn = nullptr) {
		return _erase(p_deletion_fn, [&p_value](SafeListNode *p_node) {
			return p_node->val == p_value;
		});
	}

	bool erase(const Iterator &p_iterator, DeletionFunc p_deletion_fn = nullptr) {
		if (p_iterator.cursor == nullptr) {
			return false;
		}
		return _erase(p_deletion_fn, [&p_iterator](SafeListNode *p_node) {
			if (is_marked_ptr(p_iterator.cursor->next.load(std::memory_order_acquire))) {
				return false;
			}
			return p_node == p_iterator.cursor;
		});
	}

	Iterator begin() {
		SafeListNode *node = head->next.load();
		SafeListNode *tmp = nullptr;
		do {
			if (node == nullptr) {
				break;
			}
			tmp = node->next.load();
			node = get_unmarked_ptr(tmp);
		} while (node && is_marked_ptr(tmp));
		return Iterator(node);
	}

	Iterator end() { return Iterator(nullptr); }

	// Calling this will try to advance the epoch.
	bool try_advance_epoch() {
		EBR::try_advance();
	}

	SafeList() : head(memnew_allocator(SafeListNode, A)) {}
	SafeList(std::initializer_list<T> p_init) : head(memnew_allocator(SafeListNode, A)) {
		for (const T &E : p_init) {
			insert(E);
		}
	}

	~SafeList() {
		memdelete_allocator<SafeListNode, A>(head);
		EBR::try_advance();
	}
};
