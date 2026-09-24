"""Known answers for tools/receiver_order.py: the contact-block pairing finds both orders, and rejects forwarded
receivers and halves of two different contacts."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import receiver_order as R  # noqa: E402

ENTER, STAY = 10, 11
# colliders 1, 2, 3, 4 on objects 101, 102, 103, 104; object 200 is a parent Rigidbody2D's object
GO = {1: 101, 2: 102, 3: 103, 4: 104}


def test_both_orders_are_found():
    ev = [(5, "physics", ENTER, 101, 2), (5, "physics", ENTER, 101, 2), (5, "physics", ENTER, 102, 1),   # 1 first
          (5, "physics", STAY, 104, 3), (5, "physics", STAY, 103, 4)]                                     # 3 heard second
    blocks, excluded = R.contact_blocks(ev, GO)
    assert blocks == [(5, "physics", ENTER, 1, 2), (5, "physics", STAY, 4, 3)]
    assert excluded == 0


def test_forwarded_and_mixed_halves_are_excluded():
    ev = [(6, "physics", ENTER, 101, 2), (6, "physics", ENTER, 200, 2), (6, "physics", ENTER, 102, 1),   # forwarded
          (6, "physics", STAY, 103, 4), (6, "physics", ENTER, 104, 3)]                                    # two contacts
    blocks, excluded = R.contact_blocks(ev, GO)
    assert blocks == []
    assert excluded == 2


def test_other_frames_and_unrelated_neighbours_do_not_pair():
    ev = [(7, "physics", STAY, 101, 2), (8, "physics", STAY, 102, 1),        # different frames
          (9, "physics", STAY, 101, 2), (9, "physics", STAY, 103, 4)]        # neighbours, not mirror halves
    assert R.contact_blocks(ev, GO) == ([], 0)
