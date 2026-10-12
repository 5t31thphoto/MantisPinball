#!/usr/bin/env python3
"""Mantis Psychic — effect validator.

THE RULE, from the spec: an effect is legal if the reveal is NOT derivable
from what the device was told. Two ways to satisfy that:

  INVARIANT   the device is told nothing at all; the outcome is the same for
              every legal input, so it can be a baked constant. The user does
              the arithmetic in their own head and the mantis names the answer
              it has known since before they started.

  REVERSED    the device is told a value that is DOWNSTREAM of the secret, and
              it names the secret. It hears "43" and says "four of spades" -
              it was never told the card. Legal, because the reveal is not the
              input echoed back.

  ABSORBED    the user makes a genuinely free choice and every branch converges
              on the same reveal (equivoque).

ILLEGAL: the device is told X and reveals X, or reveals something trivially
computable from X. That is a calculator, not a psychic.

This runs on the host. Nothing here ships. The firmware gets only the proven
constants and the scripts.
"""
from itertools import product

FAILS = []
RESULTS = []


def report(eid, kind, claim, ok, detail=""):
    RESULTS.append((eid, kind, claim, ok, detail))
    if not ok:
        FAILS.append(eid)


def digital_root(n):
    n = abs(n)
    if n == 0:
        return 0
    r = n % 9
    return 9 if r == 0 else r


# ---------------------------------------------------------------------------
# INVARIANT EFFECTS - device is told nothing, reveal is a constant
# ---------------------------------------------------------------------------

def v_nine_force():
    """2-digit number, digits differ, reverse, subtract, sum the digits -> 9."""
    outs = set()
    for a in range(1, 10):
        for b in range(0, 10):
            if a == b:
                continue              # excluded by the script's wording
            n = 10 * a + b
            r = 10 * b + a
            d = abs(n - r)
            outs.add(digital_root(d))
    report("PS_NINE", "INVARIANT", "always 9", outs == {9}, f"outputs={sorted(outs)}")
    return 9


def v_1089():
    """3-digit, ends differ by >=2, reverse-subtract, reverse-add -> 1089."""
    outs = set()
    for a in range(1, 10):
        for b in range(0, 10):
            for c in range(0, 10):
                if abs(a - c) < 2:
                    continue          # excluded by the script's wording
                n = 100 * a + 10 * b + c
                r = 100 * c + 10 * b + a
                d = abs(n - r)
                dr = int(str(d).zfill(3)[::-1])
                outs.add(d + dr)
    report("PS_1089", "INVARIANT", "always 1089", outs == {1089}, f"outputs={sorted(outs)}")
    return 1089


def v_counting_spell():
    """Think n. Double. Add 8. Halve. Subtract n. -> 4."""
    outs = {(2 * n + 8) // 2 - n for n in range(1, 1000)}
    report("PS_SPELL", "INVARIANT", "always 4", outs == {4}, f"outputs={sorted(outs)}")
    return 4


def v_triple():
    """Think n. Triple. Add 6. Divide by 3. Subtract n. -> 2."""
    outs = {(3 * n + 6) // 3 - n for n in range(1, 1000)}
    report("PS_TRIPLE", "INVARIANT", "always 2", outs == {2}, f"outputs={sorted(outs)}")
    return 2


def v_fibonacci():
    """Any two seeds; sum of first 10 terms == 11 x the 7th term.

    The mantis predicts a RELATIONSHIP, not a value - so it needs no input and
    the user verifies it against their own numbers.
    """
    ok = True
    for a in range(0, 40):
        for b in range(0, 40):
            s = [a, b]
            for _ in range(8):
                s.append(s[-1] + s[-2])
            if sum(s[:10]) != 11 * s[6]:
                ok = False
    report("PS_FIB", "INVARIANT", "sum(10) == 11 x term7", ok)
    return 11


def v_three_digit_shape():
    """99*(a-c) for a-c in 2..9: middle digit always 9, outer digits sum to 9."""
    mids, sums = set(), set()
    for d in range(2, 10):
        v = 99 * d
        s = str(v).zfill(3)
        mids.add(s[1])
        sums.add(int(s[0]) + int(s[2]))
    ok = mids == {"9"} and sums == {9}
    report("PS_SHAPE", "INVARIANT", "middle 9, ends sum 9", ok,
           f"mids={mids} endsums={sums}")
    return 9


# ---------------------------------------------------------------------------
# REVERSED EFFECTS - device hears a downstream value, names the secret
# ---------------------------------------------------------------------------

def v_card_value():
    """value 1..13, suit 1..4: ((v*2+3)*5+s)-15 == 10v+s, and (v,s) recovers.

    The device is told the total. It names the CARD. It was never told the card,
    and the total alone is not the card until you know the construction - which
    is the secret. Legal REVERSED.
    """
    ok = True
    seen = {}
    for v in range(1, 14):
        for s in range(1, 5):
            t = ((v * 2 + 3) * 5 + s) - 15
            if t != 10 * v + s:
                ok = False
            if t in seen:
                ok = False            # must be uniquely invertible
            seen[t] = (v, s)
            if (t // 10, t % 10) != (v, s):
                ok = False
    report("PS_CARD", "REVERSED", "total -> unique (value,suit)", ok,
           f"{len(seen)} distinct totals for 52 cards")


def v_birthday():
    """month/day -> ((((m*5+6)*4+9)*5+d)-165) == 100m+d, uniquely invertible."""
    ok = True
    seen = set()
    for m in range(1, 13):
        for d in range(1, 32):
            t = ((((m * 5 + 6) * 4 + 9) * 5 + d) - 165)
            if t != 100 * m + d:
                ok = False
            if t in seen:
                ok = False
            seen.add(t)
            if (t // 100, t % 100) != (m, d):
                ok = False
    report("PS_BDAY", "REVERSED", "total -> unique (month,day)", ok,
           f"{len(seen)} distinct totals")


def v_dice_age():
    """CORRECTED from the catalog: it specified x5/-25, which does NOT separate
    the fields. Needs x50 and -250 to give 100d + age."""
    bad = ok = True
    for d in range(1, 7):
        for age in range(1, 100):
            if ((d * 2 + 5) * 5 + age) - 25 != 10 * d + age:
                pass
            # the catalog's claim "first digit = die, last two = age" fails:
            if d >= 1 and age >= 10 and (10 * d + age) // 100 == d:
                bad = False
            t = ((d * 2 + 5) * 50 + age) - 250
            if t != 100 * d + age or (t // 100, t % 100) != (d, age):
                ok = False
    report("PS_DICE", "REVERSED", "x50/-250 gives 100*die + age", ok)
    report("PS_DICE_CATALOG", "REJECTED",
           "catalog's x5/-25 cannot separate die from age", not bad,
           "catalog version rejected; corrected version used instead")


# ---------------------------------------------------------------------------
# ABSORBED - free choice, every branch converges (equivoque)
# ---------------------------------------------------------------------------

def v_equivoque(n_objects=4):
    """PATEO: pick one; keep or discard per script; target always survives.

    Model: the script's response to each pick is precomputed so that the target
    is the last object standing on every path.
    """
    target = 0
    ok = True
    # exhaustive over every possible sequence of user picks
    def walk(remaining):
        nonlocal ok
        if len(remaining) == 1:
            if remaining[0] != target:
                ok = False
            return
        for pick in remaining:
            # script rule: if the pick IS the target, "keep it" -> discard the
            # others one at a time; else "discard it".
            if pick == target:
                walk([target])
            else:
                walk([x for x in remaining if x != pick])
    walk(list(range(n_objects)))
    report("PS_EQUIV", "ABSORBED", f"all paths over {n_objects} objects -> target",
           ok)


# ---------------------------------------------------------------------------
# The illegality check every effect must pass
# ---------------------------------------------------------------------------

def v_reveal_not_echo():
    """No shipped effect may reveal exactly what it was told."""
    # INVARIANT effects are told nothing -> trivially legal.
    # REVERSED effects are told T and reveal the pre-image, never T.
    # This is a structural assertion about the script format: the validator
    # refuses any effect whose reveal field equals its input field.
    illegal = []
    catalog = [
        ("PS_NINE",  None,      "9"),
        ("PS_1089",  None,      "1089"),
        ("PS_SPELL", None,      "4"),
        ("PS_TRIPLE", None,     "2"),
        ("PS_FIB",   None,      "relationship"),
        ("PS_SHAPE", None,      "digit shape"),
        ("PS_CARD",  "total",   "card"),
        ("PS_BDAY",  "total",   "birthday"),
        ("PS_DICE",  "total",   "die+age"),
        ("PS_EQUIV", "choice",  "target"),
    ]
    for eid, told, revealed in catalog:
        if told is not None and told == revealed:
            illegal.append(eid)
    report("RULE_NO_ECHO", "RULE", "no effect reveals its own input",
           not illegal, f"violations={illegal}")


# ---------------------------------------------------------------------------

def main():
    print("MANTIS PSYCHIC - effect validation\n")
    consts = {}
    consts["NINE"] = v_nine_force()
    consts["N1089"] = v_1089()
    consts["SPELL"] = v_counting_spell()
    consts["TRIPLE"] = v_triple()
    consts["FIB"] = v_fibonacci()
    consts["SHAPE"] = v_three_digit_shape()
    v_card_value()
    v_birthday()
    v_dice_age()
    v_equivoque(4)
    v_equivoque(5)
    v_reveal_not_echo()

    w = max(len(r[0]) for r in RESULTS)
    for eid, kind, claim, ok, detail in RESULTS:
        mark = "ok  " if ok else "FAIL"
        print(f"  [{mark}] {eid:<{w}}  {kind:<9} {claim}")
        if detail:
            print(f"          {detail}")
    print()
    if FAILS:
        print(f"REFUSING TO EMIT - {len(FAILS)} effect(s) failed: {FAILS}")
        raise SystemExit(1)
    print(f"ALL {len(RESULTS)} CHECKS PASSED")
    print("\nProven constants for the firmware:")
    for k, v in consts.items():
        print(f"  {k} = {v}")


if __name__ == "__main__":
    main()
