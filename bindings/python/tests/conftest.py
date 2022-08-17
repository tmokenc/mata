"""Shared fixtures for the testing of functionality of Python binding."""

import os
import shutil
import pytest
import mata

__author__ = 'Tomas Fiedor'


@pytest.fixture(scope="function")
def cleandir():
    """Runs the test in the clean new dir, which is purged afterwards"""
    temp_path = tempfile.mkdtemp()
    os.chdir(temp_path)
    yield
    shutil.rmtree(temp_path)


@pytest.fixture(scope="function")
def dfa_one_state_uni():
    lhs = mata.Nfa(1)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    lhs.add_trans_raw(0, 1, 0)
    lhs.add_final_state(0)
    yield lhs


@pytest.fixture(scope="function")
def dfa_one_state_empty():
    lhs = mata.Nfa(1)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    lhs.add_trans_raw(0, 1, 0)
    yield lhs


@pytest.fixture(scope="function")
def nfa_two_states_uni():
    lhs = mata.Nfa(2)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    lhs.add_trans_raw(0, 1, 0)
    lhs.add_trans_raw(0, 0, 1)
    lhs.add_trans_raw(1, 0, 1)
    lhs.add_trans_raw(1, 1, 1)
    lhs.add_final_state(1)
    yield lhs


def divisible_by(k: int):
    """
    Constructs automaton accepting strings containing ones divisible by "k"
    """
    assert k > 1
    lhs = mata.Nfa(k+1)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    for i in range(1, k + 1):
        lhs.add_trans_raw(i - 1, 1, i)
        lhs.add_trans_raw(i, 0, i)
    lhs.add_trans_raw(k, 1, 1)
    lhs.add_final_state(k)
    return lhs


@pytest.fixture(scope="function")
def fa_one_divisible_by_two():
    yield divisible_by(2)


@pytest.fixture(scope="function")
def fa_one_divisible_by_four():
    yield divisible_by(4)


@pytest.fixture(scope="function")
def fa_one_divisible_by_eight():
    yield divisible_by(8)


@pytest.fixture(scope="function")
def fa_odd_ones():
    lhs = mata.Nfa(2)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    lhs.add_trans_raw(0, 1, 1)
    lhs.add_trans_raw(1, 1, 0)
    lhs.add_trans_raw(1, 0, 1)
    lhs.add_final_state(1)


@pytest.fixture(scope="function")
def fa_even_ones():
    lhs = mata.Nfa(2)
    lhs.add_initial_state(0)
    lhs.add_trans_raw(0, 0, 0)
    lhs.add_trans_raw(0, 1, 1)
    lhs.add_trans_raw(1, 1, 0)
    lhs.add_trans_raw(1, 0, 1)
    lhs.add_final_state(0)


@pytest.fixture(scope="function")
def binary_alphabet():
    alph = mata.OnTheFlyAlphabet()
    alph.translate_symbol("0")
    alph.translate_symbol("1")
    yield alph