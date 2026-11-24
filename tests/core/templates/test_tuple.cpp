/**************************************************************************/
/*  test_tuple.cpp                                                        */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_tuple)

#include "core/templates/tuple.h"

namespace TestTuple {

TEST_CASE("[Tuple] Tuple initialization") {
	Tuple<int, uint64_t, float, double> tuple(1, 2, 3.0f, 4.0);

	CHECK_EQ(tuple.get<0>(), 1);
	CHECK_EQ(tuple.get<1>(), 2);
	CHECK_EQ(tuple.get<2>(), 3.0f);
	CHECK_EQ(tuple.get<3>(), 4.0);

	Tuple<int, uint64_t, float, double> partial(5, 6);
	CHECK_EQ(partial.get<0>(), 5);
	CHECK_EQ(partial.get<1>(), 6);

	Tuple deduced(1, 1.0f, true);
	static_assert(std::is_same_v<TupleElement<0, decltype(deduced)>, int>);
	static_assert(std::is_same_v<TupleElement<1, decltype(deduced)>, float>);
	static_assert(std::is_same_v<TupleElement<2, decltype(deduced)>, bool>);
}

#if false
TEST_CASE("[Tuple] Tuple structured bindings") {
	// Check if the reference overloads returns references.
	Tuple<int> tuplei(10);
	const auto &[tuplei_0] = tuplei;
	auto &[tuplei_1] = tuplei;
	++tuplei_1;
	CHECK_EQ(tuplei_0, 11);
	CHECK_EQ(tuplei_1, 11);
	CHECK_EQ(tuplei.get<0>(), 11);

	// Check if the bindings compile correctly.
	Tuple<int> tuple_i1(10);
	[[maybe_unused]] auto [tuple_i1_0] = tuple_i1;
	[[maybe_unused]] auto &[tuple_i1_1] = tuple_i1;
	[[maybe_unused]] const auto &[tuple_i1_2] = tuple_i1;
	[[maybe_unused]] auto &&[tuple_i1_3] = tuple_i1;

	[[maybe_unused]] auto [tuple_it1_0] = Tuple<int>(20);
	[[maybe_unused]] const auto &[tuple_it1_1] = Tuple<int>(20);
	[[maybe_unused]] auto &&[tuple_it1_2] = Tuple<int>(20);
	[[maybe_unused]] const auto &&[tuple_it1_3] = Tuple<int>(20);

	Tuple<int, float> tuple_if1(10, 10.0);
	[[maybe_unused]] auto [tuple_if1_0, tuple_if1_1] = tuple_if1;
	[[maybe_unused]] auto &[tuple_if1_2, tuple_if1_3] = tuple_if1;
	[[maybe_unused]] const auto &[tuple_if1_4, tuple_if1_5] = tuple_if1;
	[[maybe_unused]] auto &&[tuple_if1_6, tuple_if_7] = tuple_if1;

	[[maybe_unused]] auto [tuple_ift1_0, tuple_ift1_1] = Tuple<int, float>(20, 20.0);
	[[maybe_unused]] const auto &[tuple_ift1_2, tuple_ift1_3] = Tuple<int, float>(20, 20.0);
	[[maybe_unused]] auto &&[tuple_ift1_4, tuple_ift1_5] = Tuple<int, float>(20, 20.0);
	[[maybe_unused]] const auto &&[tuple_ift1_6, tuple_ift1_7] = Tuple<int, float>(20, 20.0);

	// Check if the recursive bindings compile correctly.
	Tuple<int> tuple_local(10);
	Tuple<Tuple<int>> tuple_rec1(tuple_local);
	[[maybe_unused]] auto [tuple_rec1_0] = tuple_rec1;
	[[maybe_unused]] auto &[tuple_rec1_1] = tuple_rec1;
	[[maybe_unused]] const auto &[tuple_rec1_2] = tuple_rec1;
	[[maybe_unused]] auto &&[tuple_rec1_3] = tuple_rec1;

	[[maybe_unused]] auto [tuple_rect1_0] = Tuple<Tuple<int>>(Tuple<int>(10));
	[[maybe_unused]] const auto &[tuple_rect1_1] = Tuple<Tuple<int>>(Tuple<int>(10));
	[[maybe_unused]] auto &&[tuple_rect1_2] = Tuple<Tuple<int>>(Tuple<int>(10));
	[[maybe_unused]] const auto &&[tuple_rect1_3] = Tuple<Tuple<int>>(Tuple<int>(10));
}
#endif
} //namespace TestTuple
