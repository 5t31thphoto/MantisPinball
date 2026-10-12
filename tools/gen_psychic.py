#!/usr/bin/env python3
"""Mantis Psychic — effect generator and GAP VALIDATOR.

THE RULE THIS FILE ENFORCES
---------------------------
An effect is legal only if what the device LEARNS cannot account for what it
REVEALS. Invariance is one way to get there but it is not the requirement:

  * NO-INPUT effects        the device is told nothing at all. The user does the
                            arithmetic in their head. The gap is total.
  * ABSORBED-CHOICE effects the user makes a real choice and every choice leads
                            to the same reveal. It knew before they picked.
  * DERIVATION effects      the user reports a COMPOSITE (e.g. 10*value+suit) and
                            the device names the PARTS. It never heard the card,
                            the date, the time - only a number that hides them.

  ILLEGAL: the device is told X and reveals X. That is a calculator, not a
  psychic, and the validator rejects it.

Every effect therefore carries a declared contract (learns / reveals / why the
second does not follow from the first) and the validator refuses to emit any
effect whose contract it cannot verify.

The device runtime does NO force arithmetic. All of it is proven here, offline,
and the pack ships the proven result.

Output: data/content/psychic.mpk  (plain text, parsed by src/mantis/mantis_psychic.cpp)
"""
import os, sys, itertools

# ─────────────────────────────────────────────────────────────────────────────
# PACK FORMAT (kept plain text like the other content packs)
#   @PACK  title|version
#   @FX    id|title|mode|method          mode: solo | performer
#   @GAP   learns|reveals|why
#   @P pose        set the mantis pose for following beats
#   @S text        say a patter line
#   @W ms          hold
#   @B prompt      beat: wait for a button press (tells the device NOTHING)
#   @A prompt|lo|hi|slot     ask for a value (L scrolls, R confirms) -> slot
#   @C prompt|a|b|c...       a genuine choice, absorbed (never branches)
#   @X slot|postfix-expr     compute into a slot (derivation effects only)
#   @R text                  reveal; {s0}..{s3} and {CARD s0 s1} style refs
#   @E             end of effect
# ─────────────────────────────────────────────────────────────────────────────

POSES = ["idle", "think", "gaze", "raise", "reveal", "bow"]

# ── expression evaluator (mirrors the tiny runtime one, used for proving) ────
def ev(expr, slots):
    st = []
    for tok in expr.split():
        if tok.startswith("s") and tok[1:].isdigit():
            st.append(slots[int(tok[1:])])
        elif tok.lstrip("-").isdigit():
            st.append(int(tok))
        else:
            b = st.pop(); a = st.pop()
            if   tok == "+": st.append(a + b)
            elif tok == "-": st.append(a - b)
            elif tok == "*": st.append(a * b)
            elif tok == "/":
                if b == 0: raise ZeroDivisionError(expr)
                st.append(a // b)
            elif tok == "%":
                if b == 0: raise ZeroDivisionError(expr)
                st.append(a % b)
            else: raise ValueError("bad token %r in %r" % (tok, expr))
    if len(st) != 1: raise ValueError("unbalanced expr %r" % expr)
    return st[0]

CARD_V = ["", "ACE","TWO","THREE","FOUR","FIVE","SIX","SEVEN","EIGHT","NINE","TEN",
          "JACK","QUEEN","KING"]
CARD_S = ["", "CLUBS","HEARTS","SPADES","DIAMONDS"]
MONTHS = ["", "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"]

def fmt(text, slots):
    """Resolve reveal placeholders the same way the runtime does."""
    out = text
    for i in range(4):
        out = out.replace("{s%d}" % i, str(slots[i]))
    if "{CARD" in out:
        v, s = slots[0], slots[1]
        name = "%s of %s" % (CARD_V[v] if 1 <= v <= 13 else "?",
                             CARD_S[s] if 1 <= s <= 4 else "?")
        out = out.replace("{CARD s0 s1}", name)
    if "{MONTH" in out:
        m = slots[0]
        out = out.replace("{MONTH s0}", MONTHS[m] if 1 <= m <= 12 else "?")
    if "{CLOCK" in out:
        out = out.replace("{CLOCK s0 s1}", "%d:%02d" % (slots[0], slots[1]))
    return out


class Effect:
    def __init__(self, fid, title, mode, method, learns, reveals, why):
        self.id, self.title, self.mode, self.method = fid, title, mode, method
        self.learns, self.reveals, self.why = learns, reveals, why
        self.beats = []
        self.asks = []      # (lo, hi, slot)
        self.choices = []   # option counts
        self.computes = []  # (slot, expr)
        self.reveal_txt = None
    # beat builders
    def pose(self, p):  self.beats.append(("P", p)); return self
    def say(self, t):   self.beats.append(("S", t)); return self
    def wait(self, ms): self.beats.append(("W", str(ms))); return self
    def beat(self, t):  self.beats.append(("B", t)); return self
    def ask(self, prompt, lo, hi, slot, reach=None):
        """lo..hi is the scroll UI range. `reach` is the set of values the force
        can actually yield - the validator proves over THAT, because a scroll
        range contains numbers the arithmetic can never produce."""
        self.beats.append(("A", "%s|%d|%d|%d" % (prompt, lo, hi, slot)))
        self.asks.append((lo, hi, slot, list(reach) if reach is not None
                          else list(range(lo, hi + 1))))
        return self
    def choose(self, prompt, *labels):
        self.beats.append(("C", "|".join((prompt,) + labels)))
        self.choices.append(len(labels)); return self
    def compute(self, slot, expr):
        self.beats.append(("X", "%d|%s" % (slot, expr)))
        self.computes.append((slot, expr)); return self
    def chart(self, prompt):
        """'M': a 0..99 symbol chart re-dealt every play (firmware).
        ('G' is already the gap-contract doc line the loader skips.)"""
        self.beats.append(("M", prompt)); return self
    def count_text(self, prompt, n, texts):
        """'Q': show one of `texts` at random; the reveal is its n-th word."""
        for t in texts:
            assert "|" not in t and "~" not in t, "texts may not contain | or ~"
        self.beats.append(("Q", "%s|%d|%s" % (prompt, n, "~".join(texts))))
        self.count_n, self.count_texts = n, list(texts)
        return self
    def reveal(self, t):
        self.beats.append(("R", t)); self.reveal_txt = t; return self


# ═════════════════════════════════════════════════════════════════════════════
# VALIDATOR
# ═════════════════════════════════════════════════════════════════════════════
class GapError(Exception): pass

def validate(fx):
    errs = []
    if fx.mode not in ("solo", "performer"):
        errs.append("mode must be solo|performer")
    if not fx.why.strip():
        errs.append("empty gap justification")
    if fx.reveal_txt is None:
        errs.append("no @R reveal beat")
    # slots must be assigned before use
    assigned = set()
    for kind, arg in fx.beats:
        if kind == "A": assigned.add(int(arg.split("|")[3]))
        if kind == "X":
            slot, expr = arg.split("|", 1)
            for tok in expr.split():
                if tok.startswith("s") and tok[1:].isdigit() and int(tok[1:]) not in assigned:
                    errs.append("expr reads unassigned slot %s" % tok)
            assigned.add(int(slot))
        if kind == "R":
            for i in range(4):
                if ("{s%d}" % i) in arg and i not in assigned:
                    errs.append("reveal reads unassigned slot s%d" % i)
    if errs:
        raise GapError("%s: %s" % (fx.id, "; ".join(errs)))

    # ---- the gap proof, by effect class -------------------------------------
    if not fx.asks:
        # NO-INPUT (or absorbed-choice): the device is told nothing that could
        # produce the reveal. A choice never branches in this format, so every
        # option provably lands on the same @R.
        kind = "absorbed-choice" if fx.choices else "no-input"
        return kind, 1  # one reachable outcome

    # DERIVATION: enumerate the whole declared input domain and prove that the
    # reveal is never a restatement of what the device was told.
    domains = [reach for (_lo, _hi, _s, reach) in fx.asks]
    outcomes = set()
    n = 0
    for combo in itertools.product(*domains):
        slots = [0, 0, 0, 0]
        for (_lo, _hi, slot, _r), val in zip(fx.asks, combo):
            slots[slot] = val
        try:
            for slot, expr in fx.computes:
                slots[slot] = ev(expr, slots)
        except Exception as e:
            raise GapError("%s: %s" % (fx.id, e))
        shown = fmt(fx.reveal_txt, slots)
        # ECHO CHECK: the reveal must not simply hand back what it was told.
        for val in combo:
            if shown.strip() == str(val):
                raise GapError("%s: reveal echoes the input (%s) - that is a "
                               "calculator, not a psychic" % (fx.id, val))
        if "?" in shown:
            raise GapError("%s: input %s produces a malformed reveal %r"
                           % (fx.id, combo, shown))
        outcomes.add(shown)
        n += 1
    if len(outcomes) < 2:
        raise GapError("%s: declared an input but the reveal never varies - "
                       "drop the input, it is a no-input effect" % fx.id)
    return "derivation(%d inputs -> %d reveals)" % (n, len(outcomes)), len(outcomes)


# ═════════════════════════════════════════════════════════════════════════════
# THE LIBRARY — methods, each proven, then dressed procedurally
# ═════════════════════════════════════════════════════════════════════════════

# Patter skins: the same proven method, different theatre. This is the
# "procedural" part - method x skin, so the library is large without inventing
# new mathematics for every entry.
SKINS = [
    dict(name="ORACLE",  open="The mantis stills. Something is already written.",
         mid="It does not need to see. It only waits.",
         hit="The mantis was never guessing."),
    dict(name="VOID",    open="A cold draft. The mantis turns away from you.",
         mid="It has stopped listening. It already knows.",
         hit="It knew before you began."),
    dict(name="CIRCUS",  open="The mantis takes a tiny theatrical bow.",
         mid="It taps its foreleg, impatient with the obvious.",
         hit="Ta-da. It was never in doubt."),
    dict(name="SWAMP",   open="Fog. The mantis is barely a silhouette.",
         mid="Something in the reeds already answered for you.",
         hit="The swamp told it first."),
]


# ═════════════════════════════════════════════════════════════════════════════
# SOLO - rebuilt from the player's point of view
# ═════════════════════════════════════════════════════════════════════════════
# Rules every solo trick now follows:
#   1. The player never enters anything. The mantis cannot know their number.
#   2. The reveal is DIFFERENT EVERY PLAY. The old solo tricks always ended on
#      the same word (NINE, 1089, FOUR) - a second play exposed the force. Now
#      the forced number only tells the player WHERE to look, and what is
#      there is re-dealt by the device each time.
#   3. No tedious mental arithmetic (the 7-round Kaprekar trick is gone) and
#      no reveal that is a rule instead of a reading (the Fibonacci one).
#   4. Tricks where the player types their own total are calculators in solo -
#      they moved to performer mode, where the spectator never sees the screen.

def _digits(n): return [int(c) for c in str(n)]

def prove_crystal_ball():
    """Any 2-digit n minus its digit sum is a multiple of 9 in 9..81. The
    firmware gives exactly those cells (and no others) the target symbol."""
    for n in range(10, 100):
        r = n - sum(_digits(n))
        assert 9 <= r <= 81 and r % 9 == 0, n
    return "every 2-digit start lands on a multiple of 9 (9..81)"

BOOK_PAGES = [
    # word 18 of every page is a vivid noun - the reveal must be worth hearing
    "On a quiet night the old mantis climbed the garden wall and watched as a huge yellow moon rose over the pond.",
    "Deep in the jungle a green mantis waited on a leaf while rain drummed softly on a umbrella made of giant leaves.",
    "Every single morning the young mantis practiced her dance until the busy ants stopped to watch the lantern glow at dawn.",
    "High above the quiet meadow the wind carried seeds past a sleepy mantis who dreamed of a castle made of honey.",
    "When the circus came to town the mantis sold tickets and every child begged to ride the dragon on the painted carousel.",
]
BOOK_N = 18      # digit sum of 1089

def prove_mantis_book():
    for a in range(1, 10):
        for b in range(10):
            for c in range(10):
                if a - c < 2: continue
                n = 100*a + 10*b + c; r = int(str(n)[::-1])
                d = abs(n - r); d2 = int(str(d).zfill(3)[::-1])
                assert d + d2 == 1089 and sum(_digits(d + d2)) == BOOK_N, n
    words = set()
    dull = {"the","a","an","and","to","of","on","in","at","as","it","is","was","for","with","by","his","her","its","all","who","past"}
    for page in BOOK_PAGES:
        w = page.split()
        assert len(w) >= BOOK_N, page
        word = w[BOOK_N - 1].strip(".,!?")
        assert word.lower() not in dull and len(word) >= 4, "page reveals a dull word %r: %s" % (word, page)
        words.add(word)
    assert len(words) == len(BOOK_PAGES), "every page must reveal a different word"
    return "1089 -> digit sum 18 -> word 18 of %d pages, all different: %s" % (len(BOOK_PAGES), sorted(words))

BUGS = ["LADYBUG", "BEETLE", "CRICKET", "FIREFLY", "MOTH", "DRAGONFLY", "BEE", "ANT", "SNAIL", "GRASSHOPPER"]
def _bug_orders():
    import random
    rng = random.Random(4); orders = []
    seen4 = set()
    while len(orders) < 6:
        o = BUGS[:]; rng.shuffle(o)
        if o[3] in seen4: continue
        seen4.add(o[3]); orders.append(" ".join(o))
    return orders
BUG_ORDERS = _bug_orders()

def prove_bug_jar():
    for x in range(1, 1000):
        assert (2*x + 8) // 2 - x == 4
    fourth = [o.split()[3] for o in BUG_ORDERS]
    assert len(set(fourth)) == len(BUG_ORDERS)
    return "always 4 -> 4th bug of %d jars, all different: %s" % (len(BUG_ORDERS), fourth)

def solo_crystal_ball(skin):
    fx = Effect("BALL_" + skin["name"], "THE CRYSTAL BALL", "solo", "chart_nines",
                learns="nothing at all",
                reveals="the symbol beside the number they ended on",
                why="Every 2-digit start minus its digit sum is a multiple of 9; "
                    "the device re-deals the chart each play so all of those "
                    "cells share one random symbol no other cell shows.")
    (fx.pose("gaze").say(skin["open"]).wait(900)
       .say("Think of any two-digit number.").beat("press when you have it")
       .pose("think").say("Add its two digits together.").beat("press when done")
       .say("Take that away from your number.").beat("press when you have it")
       .say("Now find your answer on my chart.").wait(700)
       .chart("press once you know your symbol")
       .pose("raise").say("Picture it. Burn it into your mind.").wait(900)
       .say(skin["mid"]).wait(800)
       .pose("reveal").reveal("{SYMBOL}").wait(700).say(skin["hit"]))
    return fx

def solo_mantis_book(skin):
    fx = Effect("BOOK_" + skin["name"], "THE MANTIS BOOK", "solo", "book_1089",
                learns="nothing at all",
                reveals="the word their number led them to",
                why="Every legal start reaches 1089, digit sum 18; the page is "
                    "picked at random each play, so the word changes every time.")
    (fx.pose("gaze").say(skin["open"]).wait(900)
       .say("Think of a three-digit number.").wait(600)
       .say("First digit two or more bigger than the last.").beat("press when you have it")
       .pose("think").say("Reverse it. Take the smaller from the larger.").beat("press when done")
       .say("Reverse THAT answer. Add the two together.").beat("press when done")
       .say("Add up the digits of your total.").beat("press when you have it")
       .say("Count that many words into my page.").wait(700)
       .count_text("press once you have your word", BOOK_N, BOOK_PAGES)
       .pose("raise").say("Your number chose that word. Not me.").wait(900)
       .pose("reveal").reveal("{WORD}").wait(700).say(skin["hit"]))
    return fx

def solo_bug_jar(skin):
    fx = Effect("JAR_" + skin["name"], "THE BUG JAR", "solo", "jar_counting",
                learns="nothing at all",
                reveals="the bug their number led them to",
                why="(2x+8)/2 - x is always 4; the jar is re-shaken each play, "
                    "so the bug in fourth place changes every time.")
    (fx.pose("idle").say(skin["open"]).wait(700)
       .say("Think of any number at all.").beat("press when you have it")
       .pose("think").say("Double it. Add eight.").beat("press when done")
       .say("Cut it in half.").beat("press when done")
       .say("Take away the number you started with.").beat("press when you have it")
       .say("Count that many bugs along my jar.").wait(700)
       .count_text("press once you have your bug", 4, BUG_ORDERS)
       .pose("raise").say("You chose freely. I only watched.").wait(900)
       .pose("reveal").reveal("{WORD}").wait(700).say(skin["hit"]))
    return fx

def solo_digital_root(skin):
    """No input at all. Any 2-digit number with unequal digits -> 9. Proven:
    10a+b - (10b+a) = 9(a-b), and the digital root of a nonzero multiple of 9
    is 9. The device is told NOTHING - the user does every step mentally."""
    fx = Effect("NINE_" + skin["name"], "THE NINE", "solo", "digital_root",
                learns="nothing at all",
                reveals="the number they ended on",
                why="No input beat carries data. The user performs every step "
                    "mentally; the device cannot know their number and does not "
                    "need to, because every legal start converges on 9.")
    (fx.pose("gaze").say(skin["open"]).wait(900)
       .say("Think of any two-digit number.").wait(700)
       .say("Make the two digits different.").beat("press when you have it")
       .pose("think").say("Reverse it. Take the smaller from the larger.")
       .beat("press when you have the answer")
       .say(skin["mid"]).wait(700)
       .say("Now add the digits of THAT together.").beat("press when you are sure")
       .pose("raise").say("You never told me a single thing.").wait(900)
       .pose("reveal").reveal("NINE").wait(600).say(skin["hit"]))
    return fx

def solo_1089(skin):
    """No input. 3-digit, ends -> 1089. Proven: 100a+10b+c-(100c+10b+a)=99(a-c);
    for a-c in 2..9 the difference plus its own reversal is always 1089."""
    fx = Effect("K1089_" + skin["name"], "THE TABLET", "solo", "force_1089",
                learns="nothing at all",
                reveals="their final total",
                why="Every step happens in the user's head. All legal starting "
                    "numbers converge on 1089, so no information needs to reach "
                    "the device for the reveal to be exact.")
    (fx.pose("gaze").say(skin["open"]).wait(900)
       .say("Think of a three-digit number.").wait(600)
       .say("First digit at least two more than the last.")
       .beat("press when you have it")
       .pose("think").say("Reverse it. Subtract the smaller from the larger.")
       .beat("press when done")
       .say("Now reverse THAT result and add the two together.")
       .beat("press when you have the total")
       .say(skin["mid"]).wait(800)
       .pose("raise").say("I have not seen a digit of it.").wait(900)
       .pose("reveal").reveal("1089").wait(600).say(skin["hit"]))
    return fx

def solo_kaprekar(skin):
    """No input. Kaprekar's routine on any 4-digit number with at least two
    distinct digits reaches 6174 within 7 iterations."""
    fx = Effect("K6174_" + skin["name"], "THE SEVEN GATES", "solo", "kaprekar",
                learns="nothing at all",
                reveals="the number they are trapped on",
                why="The routine is a proven attractor: every legal 4-digit "
                    "start falls into 6174 in at most seven passes. The device "
                    "learns nothing and still names the destination.")
    (fx.pose("gaze").say(skin["open"]).wait(800)
       .say("Think of a four-digit number.").wait(500)
       .say("Not all four digits the same.").beat("press when you have it")
       .pose("think").say("Arrange the digits biggest-to-smallest.").wait(600)
       .say("Then smallest-to-biggest. Subtract.").beat("press when done")
       .say("Do it again with your answer. And again.").wait(700)
       .say(skin["mid"]).beat("keep going until it stops changing")
       .pose("raise").say("It has stopped, hasn't it.").wait(800)
       .pose("reveal").reveal("6174").wait(600).say(skin["hit"]))
    return fx

def solo_counting(skin):
    """No input. (2x+8)/2 - x = 4 for all x."""
    fx = Effect("FOUR_" + skin["name"], "THE SMALL SPELL", "solo", "counting_4",
                learns="nothing at all",
                reveals="their result",
                why="Algebraically the user's number cancels out entirely, so "
                    "the answer is 4 whatever they chose. Nothing is entered.")
    (fx.pose("idle").say(skin["open"]).wait(700)
       .say("Think of any number at all.").beat("press when you have it")
       .pose("think").say("Double it.").wait(500).say("Add eight.").wait(500)
       .say("Cut it in half.").wait(500)
       .say("Now subtract the number you started with.")
       .beat("press when you have the answer")
       .pose("raise").say("Your number left no trace.").wait(800)
       .pose("reveal").reveal("FOUR").wait(500).say(skin["hit"]))
    return fx

def solo_fib(skin):
    """No input. Sum of 10 Fibonacci-like terms = 11 x the 7th term."""
    fx = Effect("FIB_" + skin["name"], "THE SPIRAL", "solo", "fib_eleven",
                learns="nothing at all",
                reveals="the relationship between their total and their 7th term",
                why="For any two seeds, the ten-term sum is 55a+88b = 11(5a+8b), "
                    "and 5a+8b is exactly the 7th term. The identity holds for "
                    "every pair, so nothing needs to be disclosed.")
    (fx.pose("gaze").say(skin["open"]).wait(800)
       .say("Write two numbers. Any two.").beat("press when you have them")
       .pose("think").say("Add them for the third. Keep going.").wait(600)
       .say("Ten numbers in all.").beat("press when you have ten")
       .say("Add all ten together.").beat("press when you have the total")
       .say(skin["mid"]).wait(700)
       .pose("raise").say("I never saw your two seeds.").wait(800)
       .pose("reveal").reveal("YOUR TOTAL IS ELEVEN TIMES YOUR SEVENTH NUMBER")
       .wait(700).say(skin["hit"]))
    return fx

def solo_equivoque(skin):
    """Absorbed choice. The user genuinely picks; every option converges."""
    fx = Effect("RUNE_" + skin["name"], "THE THREE RUNES", "solo", "equivoque",
                learns="which rune they touched",
                reveals="the rune it had already chosen",
                why="The choice is real but structurally absorbed - every option "
                    "leads to the same reveal, which was fixed before the user "
                    "chose. Knowing the pick cannot produce the reveal because "
                    "the reveal does not depend on it.")
    (fx.pose("gaze").say(skin["open"]).wait(800)
       .say("Three runes. No hints, no pressure.").wait(600)
       .choose("touch one", "THORN", "MOON", "ASH")
       .pose("think").say("Hold it in your mind.").wait(700)
       .say(skin["mid"]).wait(700)
       .pose("raise").say("I marked one before you woke me.").wait(900)
       .pose("reveal").reveal("THE MANTIS EYE WAS BEHIND ALL THREE")
       .wait(600).say(skin["hit"]))
    return fx

def solo_card_derivation(skin):
    """DERIVATION. They report 10v+s. The device names the CARD - which they
    never stated. Gap: a single number conceals two facts."""
    fx = Effect("CARD_" + skin["name"], "THE BURIED CARD", "solo", "card_10v_s",
                learns="one number: ten times the value plus the suit code",
                reveals="the exact card - value AND suit",
                why="The user reports a single composite number and never names "
                    "a card. The device unpacks two independent facts (value and "
                    "suit) from a figure that states neither, so the reveal is "
                    "strictly more than it was told.")
    (fx.pose("gaze").say(skin["open"]).wait(700)
       .say("Think of a card. Ace through King.").wait(600)
       .say("Clubs 1, hearts 2, spades 3, diamonds 4.")
       .beat("press when you have your card")
       .pose("think").say("Double the value. Add three. Times five.").wait(600)
       .say("Now add your suit number.").beat("press when you have the answer")
       .ask("scroll to your answer", 26, 149, 0,
            reach=[10*v + su + 15 for v in range(1, 14) for su in range(1, 5)])
       .compute(1, "s0 15 - 10 %")          # suit  = (n-15) % 10
       .compute(0, "s0 15 - 10 /")          # value = (n-15) / 10
       .pose("raise").say("You never named a card.").wait(800)
       .pose("reveal").reveal("{CARD s0 s1}").wait(600).say(skin["hit"]))
    return fx

def solo_birthday_derivation(skin):
    """DERIVATION. They report 100m+d; device names the date."""
    fx = Effect("BDAY_" + skin["name"], "THE DATE BENEATH", "solo", "bday_100m_d",
                learns="one number produced by a chain of arithmetic",
                reveals="the month and the day of their birthday",
                why="The reported figure names neither a month nor a day. The "
                    "device separates two facts out of one number the user "
                    "believes is meaningless, so the reveal exceeds the input.")
    (fx.pose("gaze").say(skin["open"]).wait(700)
       .say("Your birthday. Month number, and the day.")
       .beat("press when you have both")
       .pose("think").say("Month times five. Add six. Times four.").wait(600)
       .say("Add nine. Times five. Add the day.").wait(600)
       .say("Subtract one hundred and sixty five.")
       .beat("press when you have the answer")
       .ask("scroll to your answer", 101, 1231, 0,
            reach=[100*m + d for m in range(1, 13) for d in range(1, 32)])
       .compute(1, "s0 100 %")
       .compute(0, "s0 100 /")
       .pose("raise").say("That number said nothing about a date.").wait(800)
       .pose("reveal").reveal("{MONTH s0} {s1}").wait(600).say(skin["hit"]))
    return fx

def solo_clock_derivation(skin):
    """DERIVATION. They report 100h+m; device names the time."""
    fx = Effect("HOUR_" + skin["name"], "THE STOPPED CLOCK", "solo", "clock_100h_m",
                learns="one number",
                reveals="the hour and the minute they were holding",
                why="One figure conceals two: the device recovers an hour and a "
                    "minute from a number that names neither.")
    (fx.pose("gaze").say(skin["open"]).wait(700)
       .say("Picture a time. An hour, and minutes.")
       .beat("press when you see it")
       .pose("think").say("Hour times two. Add five. Times fifty.").wait(600)
       .say("Add the minutes. Subtract two fifty.")
       .beat("press when you have the answer")
       .ask("scroll to your answer", 100, 1259, 0,
            reach=[100*h + mi for h in range(1, 13) for mi in range(0, 60)])
       .compute(1, "s0 100 %")
       .compute(0, "s0 100 /")
       .pose("raise").say("A clock has no business in that number.").wait(800)
       .pose("reveal").reveal("{CLOCK s0 s1}").wait(600).say(skin["hit"]))
    return fx

# ── performer mode: tricks it TEACHES you to do to someone else ─────────────
def perf_21card():
    fx = Effect("P_21CARD", "THE 21 CARD DEAL", "performer", "teach_21",
                learns="which column they point to, three times",
                reveals="their card",
                why="Performer mode: the device is your accomplice, not the mind "
                    "reader. The spectator never tells YOU the card - only a "
                    "column - and three columns cannot name one of 21 cards "
                    "except through the deal's structure.")
    (fx.pose("idle").say("A self-working classic. No skill needed.").wait(800)
       .beat("press to learn it")
       .pose("think").say("Deal 21 cards face up: 3 columns of 7.").wait(900)
       .say("Ask them to think of one card. Don't touch it.").wait(900)
       .beat("next")
       .say("Ask which column it's in.").wait(700)
       .say("Gather the columns - THEIRS GOES IN THE MIDDLE.").wait(1000)
       .beat("next")
       .say("Re-deal 3 columns of 7. Ask again. Middle again.").wait(900)
       .say("Do it a third time.").beat("next")
       .pose("raise").say("Their card is now the 11th you deal.").wait(900)
       .say("Count to eleven and turn it over.").wait(800)
       .pose("reveal").reveal("ALWAYS THE ELEVENTH CARD").wait(600)
       .say("The deal does the work. You just count."))
    return fx

def perf_gilbreath():
    fx = Effect("P_GILBREATH", "THE HONEST SHUFFLE", "performer", "teach_gilbreath",
                learns="nothing from the spectator",
                reveals="that every pair splits one red one black",
                why="Performer mode. The spectator shuffles genuinely; the "
                    "Gilbreath principle guarantees the pairing regardless, so "
                    "there is nothing to learn and nothing to fake.")
    (fx.pose("idle").say("They shuffle. It still works. Really.").wait(800)
       .beat("press to learn it")
       .pose("think").say("Set up: alternate red, black, red, black.").wait(900)
       .say("All 52, strictly alternating.").beat("next")
       .say("Hand it over. Let them cut it anywhere.").wait(800)
       .say("Have them deal some cards to the table.").wait(800)
       .say("That reverses that packet - it's the secret.").beat("next")
       .say("Now let them riffle the two piles together.").wait(900)
       .say("A genuine, messy shuffle. Their hands.").beat("next")
       .pose("raise").say("Deal the deck in PAIRS.").wait(800)
       .pose("reveal").reveal("EVERY PAIR: ONE RED, ONE BLACK").wait(700)
       .say("Twenty-six times running. It cannot fail."))
    return fx

def perf_nim():
    fx = Effect("P_NIM", "THE MATCH GAME", "performer", "teach_nim",
                learns="how many they take each turn",
                reveals="that you always take the last one",
                why="Performer mode. It is a strategy, openly stated: keep each "
                    "round to five and you cannot lose. Nothing is hidden from "
                    "the device and nothing needs to be.")
    (fx.pose("idle").say("A bar bet you will never lose.").wait(800)
       .beat("press to learn it")
       .pose("think").say("Put out 20 matches. Or coins. Anything.").wait(900)
       .say("Each turn, a player takes 1, 2, 3 or 4.").wait(800)
       .say("Whoever takes the last one wins.").beat("next")
       .say("Let THEM go first. Always.").wait(800)
       .pose("raise").say("Then make every round total five.").wait(900)
       .say("They take 1, you take 4. They take 3, you take 2.").wait(1000)
       .beat("next")
       .pose("reveal").reveal("KEEP EACH ROUND AT FIVE AND THE LAST IS YOURS")
       .wait(700).say("Twenty is four fives. The maths is doing it."))
    return fx

def perf_1089_prediction():
    fx = Effect("P_1089", "THE SEALED TOTAL", "performer", "teach_1089",
                learns="nothing from the spectator",
                reveals="a total written before they started",
                why="Performer mode. You write 1089 in advance; every legal "
                    "start converges there, so the spectator's choices are free "
                    "and the prediction still matches.")
    (fx.pose("idle").say("Write a prediction. Fold it. Hand it over.").wait(900)
       .beat("press to learn it")
       .pose("think").say("Write 1089 on the paper. Seal it.").wait(900)
       .say("Now: any three-digit number, they choose.").wait(800)
       .say("First digit two or more bigger than the last.").beat("next")
       .say("Reverse. Subtract smaller from larger.").wait(800)
       .say("Reverse that answer. Add the two.").beat("next")
       .pose("raise").say("Have them open your prediction.").wait(900)
       .pose("reveal").reveal("1089 - EVERY TIME").wait(700)
       .say("You never saw a number they picked."))
    return fx

def perf_magic_square():
    """Rewritten from correct maths - the catalog's version was abandoned
    mid-derivation. Lo Shu (sum 15) plus (T-15)/3 in every cell preserves all
    rows, columns and both diagonals."""
    fx = Effect("P_SQUARE", "THE SUM SQUARE", "performer", "teach_square",
                learns="the target sum they name",
                reveals="a square where every line hits their target",
                why="Performer mode. They name the target aloud, so the device "
                    "knowing it is not a leak - the astonishment is the "
                    "construction, not secret knowledge.")
    (fx.pose("idle").say("They pick a total. You build the square.").wait(900)
       .beat("press to learn it")
       .pose("think").say("Memorise this grid: 8 1 6 / 3 5 7 / 4 9 2.").wait(1100)
       .say("Every line sums to fifteen.").beat("next")
       .say("Ask for a total divisible by three.").wait(800)
       .say("Subtract fifteen. Divide by three.").wait(800)
       .say("Add THAT to all nine cells.").beat("next")
       .pose("raise").say("Example: they say 30.").wait(700)
       .say("30-15=15, over 3 is 5. Add 5 to each cell.").wait(1000)
       .say("13 6 11 / 8 10 12 / 9 14 7.").beat("next")
       .pose("reveal").reveal("EVERY ROW COLUMN AND DIAGONAL HITS THEIR NUMBER")
       .wait(700).say("Nine sums, all correct, built in your head."))
    return fx

def perf_kruskal():
    fx = Effect("P_KRUSKAL", "THE CONVERGING PATH", "performer", "teach_kruskal",
                learns="nothing",
                reveals="that two freely chosen paths meet",
                why="Performer mode, and honestly framed: this one is about "
                    "eighty-five percent, not certain. The device says so, and "
                    "teaches the out for when it misses.")
    (fx.pose("idle").say("This one is NOT certain. About 85 percent.").wait(1000)
       .say("So you learn the out too.").beat("press to learn it")
       .pose("think").say("Deal the deck face up in a long row.").wait(900)
       .say("They pick a secret start: any of the first ten.").wait(900)
       .beat("next")
       .say("Count that many along. Land on a card.").wait(800)
       .say("Its value is the next count. Repeat to the end.").wait(1000)
       .beat("next")
       .say("You do the same from card ONE. Silently.").wait(900)
       .pose("raise").say("Your chains almost always collide.").wait(900)
       .beat("next")
       .pose("reveal").reveal("NAME THE CARD YOUR OWN CHAIN ENDED ON").wait(800)
       .say("If it misses: 'your path resisted me' and deal again."))
    return fx



# ── performer derivations: the spectator says a total, you name the secret ──
# In solo these were calculators (the player typed their own arithmetic). In
# performer mode the spectator never sees the screen, so decoding a spoken
# total into their card / birthday / time reads as genuine mind reading.
def perf_card():
    fx = Effect("P_CARD", "NAME THEIR CARD", "performer", "card_10v_s",
                learns="one total, said aloud",
                reveals="the card they are holding",
                why="Performer mode: the spectator says a total; (v*2+3)*5+s-15 "
                    "is 10v+s, decoded on a screen they cannot see.")
    (fx.pose("idle").say("They pick any card and hide it.").beat("next")
       .say("SAY: Ace is 1, Jack 11, Queen 12, King 13.").beat("next")
       .say("SAY: Double it. Add three. Times five.").beat("next")
       .say("SAY: Add 1 clubs, 2 hearts, 3 spades, 4 diamonds. Your total?").beat("next")
       .ask("type the total they said", 26, 149, 0,
            reach=sorted({(v*2+3)*5+su for v in range(1,14) for su in range(1,5)}))
       .compute(1, "s0 15 - 10 %").compute(0, "s0 15 - 10 /")
       .pose("reveal").reveal("{CARD s0 s1}").wait(600)
       .say("Pause. Stare. Then name it slowly."))
    return fx

def perf_birthday():
    fx = Effect("P_BDAY", "NAME THEIR BIRTHDAY", "performer", "bday_100m_d",
                learns="one total, said aloud",
                reveals="their birthday",
                why="Performer mode: ((((m*5+6)*4+9)*5+d)-165) is 100m+d.")
    (fx.pose("idle").say("SAY: Birth month as a number. Times five.").beat("next")
       .say("SAY: Add six. Times four. Add nine.").beat("next")
       .say("SAY: Times five. Add the day you were born.").beat("next")
       .say("SAY: Subtract 165. What do you get?").beat("next")
       .ask("type the total they said", 101, 1231, 0,
            reach=sorted({100*m + d for m in range(1,13) for d in range(1,32)}))
       .compute(1, "s0 100 %").compute(0, "s0 100 /")
       .pose("reveal").reveal("{MONTH s0} {s1}").wait(600)
       .say("Say the month first. Let the day land."))
    return fx

def perf_clock():
    fx = Effect("P_CLOCK", "NAME THEIR TIME", "performer", "clock_100h_m",
                learns="one total, said aloud",
                reveals="the time they pictured",
                why="Performer mode: ((h*2+5)*50+m)-250 is 100h+m.")
    (fx.pose("idle").say("SAY: Picture a clock. Any hour, any minute.").beat("next")
       .say("SAY: Hour times two. Add five. Times fifty.").beat("next")
       .say("SAY: Add the minutes. Subtract 250. Total?").beat("next")
       .ask("type the total they said", 100, 1259, 0,
            reach=sorted({100*h + mi for h in range(1,13) for mi in range(60)}))
       .compute(1, "s0 100 %").compute(0, "s0 100 /")
       .pose("reveal").reveal("{CLOCK s0 s1}").wait(600)
       .say("Tell them the hands. Then the minutes."))
    return fx

def perf_lightning_sum():
    fx = Effect("P_FIB", "LIGHTNING SUM", "performer", "fib_eleven",
                learns="nothing",
                reveals="how to add ten numbers instantly",
                why="Performer mode: for any seeds, the sum of the first ten "
                    "terms is exactly 11 x the 7th term.")
    (fx.pose("idle").say("They write two numbers, one under the other.").beat("next")
       .say("Each new number = the two above added.").beat("next")
       .say("Ten numbers. Then you glance once.").beat("next")
       .pose("think").say("Take the SEVENTH number. Times eleven.").beat("next")
       .say("Times 11: write it, then add it shifted one place.").beat("next")
       .pose("reveal").reveal("TOTAL = 7TH x 11").wait(700)
       .say("Write your total down BEFORE they finish adding."))
    return fx


def build_library():
    fx = []
    # Solo: three tricks, four voices each. The mantis picks one at random,
    # and every reveal is re-dealt on the device, so no two plays match.
    for skin in SKINS:
        fx += [solo_crystal_ball(skin), solo_mantis_book(skin), solo_bug_jar(skin)]
    fx += [perf_21card(), perf_gilbreath(), perf_nim(), perf_1089_prediction(),
           perf_magic_square(), perf_kruskal(),
           perf_card(), perf_birthday(), perf_clock(), perf_lightning_sum()]
    return fx


def emit_header(pack_text, path):
    """The pack, compiled into the firmware. Psychic no longer depends on the
    content partition, so the tricks always match the firmware that runs them
    (a board flashed without content - or with an older pack - used to index
    zero effects and bounce straight back to the welcome screen)."""
    lines = ["// psychic_pack.h - GENERATED by tools/gen_psychic.py. Do not edit.",
             "#pragma once", "#include <stdint.h>",
             "static const char PSY_PACK[] ="]
    for ln in pack_text.split("\n"):
        esc = ln.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('    "%s\\n"' % esc)
    lines.append("    ;")
    lines.append("static const uint32_t PSY_PACK_LEN = sizeof(PSY_PACK) - 1;")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

def emit(effects, path):
    lines = ["@PACK Mantis Psychic|1",
             "# GENERATED by tools/gen_psychic.py - every effect below passed the",
             "# gap validator: what the device is told cannot account for what it",
             "# reveals. Do not hand-edit; regenerate."]
    for f in effects:
        lines.append("@FX %s|%s|%s|%s" % (f.id, f.title, f.mode, f.method))
        lines.append("@GAP %s|%s|%s" % (f.learns, f.reveals, f.why))
        for kind, arg in f.beats:
            lines.append("@%s %s" % (kind, arg))
        lines.append("@E")
    open(path, "w").write("\n".join(lines) + "\n")
    return len("\n".join(lines))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "data", "content", "psychic.mpk")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    effects = build_library()
    print("PROVING the solo forces")
    for proof in (prove_crystal_ball, prove_mantis_book, prove_bug_jar):
        print("  %-22s %s" % (proof.__name__[6:], proof()))
    # the firmware loads one effect into fixed buffers - never ship one that
    # would be truncated on the device
    src = open(os.path.join(root, "src", "mantis", "mantis_psychic.cpp"), encoding="utf-8").read()
    import re as _re
    mp_buf = int(_re.search(r"MP_BUF\s*=?\s*(\d+)", src).group(1))
    mp_beats = int(_re.search(r"MP_MAX_BEATS\s*=?\s*(\d+)", src).group(1))
    mp_line = int(_re.search(r"MP_LINE\s*=?\s*(\d+)", src).group(1))
    reserved = set("GEF")            # loader treats these as docs / effect boundaries
    for f in effects:
        for k, a in f.beats:
            if k in reserved:
                raise SystemExit("%s: beat kind %r is reserved by the loader" % (f.id, k))
            if len(a) + 4 >= mp_line:
                raise SystemExit("%s: a %r line is %d B; the firmware reads %d B lines"
                                 % (f.id, k, len(a) + 3, mp_line))
    for f in effects:
        size = sum(len(k) + len(a) + 3 for k, a in f.beats)
        if size >= mp_buf or len(f.beats) > mp_beats:
            raise SystemExit("%s: %d bytes / %d beats exceeds firmware limits %d / %d"
                             % (f.id, size, len(f.beats), mp_buf, mp_beats))
    print("VALIDATING %d effects against the gap rule" % len(effects))
    print("-" * 68)
    solo = perf = 0
    worst = 0
    for f in effects:
        kind, _n = validate(f)          # raises GapError and aborts the build
        size = sum(len(k) + len(a) + 3 for k, a in f.beats)
        worst = max(worst, size)
        if f.mode == "solo": solo += 1
        else: perf += 1
        print("  %-18s %-9s %-34s %4d B" % (f.id, f.mode, kind, size))
    print("-" * 68)
    n = emit(effects, out)
    print("PASS: %d effects (%d solo, %d performer)" % (len(effects), solo, perf))
    print("largest single effect: %d bytes (runtime buffer must exceed this)" % worst)
    print("wrote %s (%d bytes)" % (out, n))

    # negative control: prove the validator actually rejects a calculator
    bad = Effect("BAD_ECHO", "ECHO", "solo", "echo",
                 learns="their number", reveals="their number",
                 why="none")
    bad.ask("enter your number", 1, 9, 0).reveal("{s0}")
    try:
        validate(bad)
        print("VALIDATOR BROKEN: it accepted an echo effect", file=sys.stderr)
        return 1
    except GapError as e:
        print("negative control correctly rejected: %s" % e)
    return 0

if __name__ == "__main__":
    sys.exit(main())
