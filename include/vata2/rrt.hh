/* rrt.hh -- restricted register transducer (over finite words):
 *           a restricted version of a (nondeterministic) register automaton
 *           that still has some decidable properties and closure under
 *           operations
 *
 * Copyright (c) 2020 Ondrej Lengal <ondra.lengal@gmail.com>
 *
 * This file is a part of libvata2.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _VATA2_RRT_HH_
#define _VATA2_RRT_HH_

#include <vata2/nfa.hh>

namespace Vata2
{
namespace Rrt
{

using State = Vata2::Nfa::State;
using Symbol = Vata2::Nfa::Symbol;

/// A transition of a 2-tape RRT (FIXME: probably too specialized)
struct Trans
{
	struct Guard
	{ // {{{
		enum class GuardType
		{
			IN1_VAR,    // input tape 1 has a variable
			IN2_VAR,    // input tape 2 has a variable
			IN1_EQ,     // input tape 1 has a symbol equal to a register
			IN2_EQ,     // input tape 2 has a symbol equal to a register
			IN1_NEQ,    // input tape 1 has a symbol not equal to a register
			IN2_NEQ,    // input tape 2 has a symbol not equal to a register
			IN1_IS,     // input tape 1 has a given symbol
			IN2_IS,     // input tape 2 has a given symbol
			IN1_ISNOT,  // input tape 1 doesn't have a given symbol
			IN2_ISNOT,  // input tape 2 doesn't have a given symbol
			INS_EQ,     // input tapes have the same symbol
			INS_NEQ     // input tapes have a distinct symbol
		};

		GuardType type;
		Symbol val;     // although of type Symbol, it can also store register name
	}; // Guard }}}

	struct Update
	{ // {{{
		enum class UpdateType
		{
			REG_STORE_IN1, // store input tape 1 into a given register
			REG_STORE_IN2, // store input tape 2 into a given register
			AUX_STORE_IN1, // store input tape 1 into a given auxiliary memory
			AUX_STORE_IN2, // store input tape 2 into a given auxiliary memory
			REG_CLEAR,     // clear a given register
			AUX_CLEAR      // clear a given auxiliary memory
		};

		UpdateType type;
		Symbol val;        // register or auxiliary memory name (FIXME: smaller data type)
	}; // Update }}}

	struct Output
	{ // {{{
		enum class OutputType
		{
			PUT_REG,    // output register value
			PUT_AUX,    // output auxiliary memory value
			PUT_IN1,    // output the input tape 1
			PUT_IN2     // output the input tape 2
		};

		OutputType type;
		Symbol val;        // register or auxiliary memory name (FIXME: smaller data type)
	}; // Output }}}

	using GuardList = std::list<Guard>;
	using UpdateList = std::list<Update>;

  struct Label
  {	// {{{
    GuardList guards;
    UpdateList updates;
    Output out1;  // output tape 1 action
    Output out2;  // output tape 2 action

    Label() : guards(), updates(), out1(), out2() { }
    Label(
      const GuardList&   guards,
      const UpdateList&  updates,
      const Output&      out1,
      const Output&      out2)
    : guards(guards), updates(updates), out1(out1), out2(out2)
    { }

  }; // Label }}}

	State src;
  Label lbl;
	State tgt;

  // Constructors
  Trans() : src(), lbl(), tgt() { }
	Trans(
		State          src,
		const Label&   lbl,
		State          tgt)
	: src(src), lbl(lbl), tgt(tgt)
	{ }

	Trans(
		State              src,
		const GuardList&   guards,
		const UpdateList&  updates,
		const Output&      out1,
		const Output&      out2,
		State              tgt)
	: Trans(src, Label(guards, updates, out1, out2), tgt)
	{ }

	bool operator==(const Trans& rhs) const
	{ // {{{
		// return src == rhs.src && symb == rhs.symb && tgt == rhs.tgt;
		assert(&rhs);
		return false;
    assert(false);
	} // operator== }}}
	bool operator!=(const Trans& rhs) const { return !this->operator==(rhs); }
}; // Trans }}}

// the post here is more complex than, e.g., for NFAs due to the complicated
// structure of Label, for which it is not easy to make a hash table; so far,
// we use a list
using PostSymb = std::list<std::pair<Trans::Label, State>>;        /// post over a symbol
using StateToPostMap = std::unordered_map<State, PostSymb>; /// transitions

///  A 2-tape RRT
struct Rrt
{ // {{{
private:

	// private transitions in order to avoid the use of transitions.size() which
	// returns something else than expected (basically returns the number of
	// states with outgoing edges in the RRT)
	StateToPostMap transitions = {};

public:

	void add_trans(
		State                 src,
		const Trans::Label&   lbl,
		State                 tgt);
	void add_trans(const Trans& trans) { this->add_trans(trans.src, trans.lbl, trans.tgt); }
	void add_trans(
		State                     src,
		const Trans::GuardList&   guards,
		const Trans::UpdateList&  updates,
		const Trans::Output&      out1,
		const Trans::Output&      out2,
		State                     tgt)
	{ // {{{
		this->add_trans(src, Trans::Label(guards, updates, out1, out2), tgt);
	} // }}}

}; // Rrt }}}

// CLOSING NAMESPACES AND GUARDS
} /* Rrt */
} /* Vata2 */

namespace std
{ // {{{
template <>
struct hash<Vata2::Rrt::Trans>
{
	inline size_t operator()(const Vata2::Rrt::Trans& trans) const
	{
		size_t accum = std::hash<Vata2::Nfa::State>{}(trans.src);
		assert(false);
		accum = Vata2::util::hash_combine(accum, trans.tgt);
		return accum;
	}
};

std::ostream& operator<<(std::ostream& strm, const Vata2::Nfa::Trans& trans);
} // std }}}

#endif /* _VATA2_RRT_HH_ */
