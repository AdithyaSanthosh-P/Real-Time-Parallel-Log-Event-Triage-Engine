#!/usr/bin/env python3
"""Generate tests/basepaper/labeled.tsv, the labeled input set for Phase R (RB2, RB4).

The base paper's own 1,250-sample dataset is not available, so this builds a stand-in of
the same size from templates. Lines are tab-separated:

    text <TAB> labels <TAB> template

`labels` is the ground truth: a comma-separated subset of EMAIL PHONE QUESTION GREETING NAME,
or NONE. It describes what the line *is*, decided by the template it was built from, never
by running the regexes. So some lines are deliberately labelled as the paper's regexes would
NOT see them (a capitalised "Hello everyone" is a GREETING; "john smith" is a NAME; "what is
your name" is a QUESTION). Those are the findings RB4 measures. Fixed seed: two runs write
byte-identical files. Lines are ASCII, contain no tab, and are at most 256 characters.

Templates (id: label  count  what it generates)

  EMAIL  (200)
    E1  EMAIL     120  local@domain.tld; local is first.last, first_last, first+tag or first<digits>
    E2  EMAIL      40  the same shape in upper case (JOHN.SMITH@EXAMPLE.COM)
    E3  EMAIL      40  sub-domain addresses (a@mail.corp.example.co.in)
  PHONE  (180)
    P1  PHONE     120  ten digits, first digit 6-9 (Indian mobile format)
    P2  PHONE      30  the same number grouped with a space (98765 43210)
    P3  PHONE      30  the same number with a +91 prefix
  QUESTION  (180)
    Q1  QUESTION  110  full question ending in '?'
    Q2  QUESTION   40  the same kind of question written without '?'
    Q3  QUESTION   15  question followed by a trailing space after '?'
    Q4  QUESTION   15  one-word questions (Why?, Really?)
  GREETING  (150)
    G1  GREETING   70  lower-case greeting at the start (hello everyone, good morning all)
    G2  GREETING   50  capitalised greeting (Hello everyone, Good Morning all)
    G3  GREETING   30  greeting with punctuation (Hello, John!, hi!)
  NAME  (150)
    N1  NAME      100  "First Last", both capitalised
    N2  NAME       25  all lower case (john smith)
    N3  NAME       25  three-part, hyphenated, initial or apostrophe names (Mary-Anne Smith)
  MULTI  (120)  lines that are two or three categories at once
    M1  GREETING,EMAIL            30  "hello <email>"
    M2  GREETING,QUESTION         30  "hi, what is your name?"
    M3  PHONE,EMAIL               20  "reach me on <phone> or <email>"
    M4  QUESTION,EMAIL            15  "is <email> your email?"
    M5  PHONE,QUESTION            15  "is <phone> your number?"
    M6  GREETING,PHONE            10  "hey, call <phone>"
  NONE  (270)  lines that are none of the five categories
    X1  NONE       70  ordinary statements
    X2  NONE       40  malformed emails (john@, @example.com, john@example, john@@example.com ...)
    X3  NONE       55  numbers that are not mobile numbers: 11 digits (20), 9 digits (10),
                       10 digits starting with 1-5 (25)
    X4  NONE       30  a greeting word in the middle of a sentence (she said hello and left)
    X5  NONE       25  short non-matching tokens (invalid-email, ok, n/a)
    X6  NONE       30  two capitalised words that are not a person (New Delhi, Monday Morning)
    X7  NONE       20  statements with digits (Order 12345 shipped.)

Total 1,250. Usage: gen_bp.py [OUT]   (default tests/basepaper/labeled.tsv; '-' = stdout)
"""
import collections
import random
import sys

SEED = 20261012
CATS = ["EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"]

FIRST = ["John", "Mary", "Karuna", "Anvesha", "Adithya", "Priya", "Rahul", "Anita", "Vikram",
         "Sneha", "Arjun", "Meera", "Kiran", "Divya", "Rohan", "Neha", "Sanjay", "Pooja",
         "David", "Emma", "Liam", "Olivia", "Noah", "Sophia", "Lucas", "Mia", "Ethan", "Zoe"]
LAST = ["Smith", "Kumar", "Sharma", "Patel", "Singh", "Iyer", "Reddy", "Nair", "Gupta",
        "Johnson", "Brown", "Davis", "Miller", "Wilson", "Moore", "Taylor", "Anderson",
        "Thomas", "Jackson", "White", "Harris", "Martin", "Lee", "Walker", "Young", "Menon"]
DOMAINS = ["example", "company", "test", "mail", "corp", "university", "service", "webmail",
           "startup", "college", "office", "bank", "shop", "cloud"]
TLDS = ["com", "org", "net", "in", "edu", "io", "co.uk", "co.in", "ac.in"]
SUBS = ["mail", "corp", "staff", "eng", "hr", "support", "dev"]

THINGS = ["name", "email address", "phone number", "address", "order number", "password",
          "account", "schedule", "budget", "deadline", "invoice", "manager", "team", "project"]
VERBS = ["reset", "change", "update", "verify", "cancel", "find", "download", "share"]
STEMS = [
    "What is your {t}", "What is the status of my {t}", "Where can I find my {t}",
    "How do I {v} my {t}", "Can you {v} the {t}", "Could you tell me my {t}",
    "Who is responsible for the {t}", "When will the {t} be ready", "Why was my {t} {v}d",
    "Which {t} should I use", "Is there a way to {v} the {t}", "Do you know the {t}",
    "Are you able to {v} my {t}", "Will the {t} change tomorrow", "How long does it take to {v} a {t}",
]
ONE_WORD_Q = ["Why", "Really", "Seriously", "Now", "Where", "Who", "When", "How", "Sure", "Again",
              "Today", "Tomorrow", "Really now", "Why not", "So what"]

GREET_LOWER = ["hello", "hi", "hey", "good morning", "good afternoon"]
GREET_TAIL = ["", " everyone", " there", " team", " all", " friends", " sir", " madam", " folks",
              " world", " class", " guys", " again", " buddy"]

STATEMENTS = [
    "the weather is nice today", "the meeting starts at noon", "please send the report by friday",
    "the server was restarted last night", "thanks for your help", "see you tomorrow",
    "the package arrived this morning", "lunch is at one", "the printer is out of paper",
    "we shipped the new version", "the document is attached", "I will be late today",
    "the build passed on the first try", "nothing changed since yesterday",
    "all tests are green", "the room is booked for tuesday", "he went home early",
    "the library closes at eight", "we need more coffee", "the update is installed",
    "she finished the draft", "the file is in the shared folder", "traffic was heavy this morning",
    "the invoice has been paid", "I am working from home", "the patch fixes the crash",
    "it rained all weekend", "the train was on time", "we reviewed the design together",
    "the results look fine",
]
STATEMENT_EXTRA = ["", " again", " as planned", " for now", " this week", " after lunch",
                   " as usual", " before the demo"]
MID_GREET = [
    "she said {g} and left", "he waved and said {g} to the guests", "the sign says {g} in five languages",
    "they shouted {g} across the hall", "I wrote {g} on the card", "the parrot only says {g}",
    "do not forget to say {g} to the new hire", "the kids practised saying {g} today",
]
TOKENS = ["invalid-email", "ok", "n/a", "none", "test", "asdf", "lorem ipsum", "foo bar baz",
          "yes", "no", "todo", "tbd", "---", "...", "12:30", "v2.1.4", "404", "x", "abc123",
          "world-notes.txt", "readme.md", "main.c", "a_b_c", "100%", "#1"]
NOT_NAMES = ["New Delhi", "Monday Morning", "Red Fort", "Happy Hour", "Black Friday", "Ice Cream",
             "Main Street", "Silver Spring", "Blue Moon", "Green Park", "Golden Gate", "Table Tennis",
             "Hot Chocolate", "Sunday Market", "Open Source", "Dark Mode", "Fire Alarm",
             "Cold Storage", "Night Shift", "Pine Street", "Lake View", "North Gate", "Coffee Break",
             "Service Desk", "Power Cut", "Mountain View", "Rush Hour", "Water Tank", "Fast Food",
             "Final Exam"]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.seen = set()
        self.rows = []          # (text, labels, template)

    def pick(self, seq):
        return seq[self.r.randrange(len(seq))]

    def digits(self, n):
        return "".join(str(self.r.randrange(10)) for _ in range(n))

    def phone(self):
        return str(6 + self.r.randrange(4)) + self.digits(9)

    def email(self, upper=False, sub=False):
        f, l = self.pick(FIRST).lower(), self.pick(LAST).lower()
        form = self.r.randrange(4)
        local = [f"{f}.{l}", f"{f}_{l}", f"{f}+{self.pick(['news', 'work', 'spam', 'bills'])}",
                 f"{f}{self.digits(2 + self.r.randrange(3))}"][form]
        dom = f"{self.pick(SUBS)}.{self.pick(DOMAINS)}" if sub else self.pick(DOMAINS)
        e = f"{local}@{dom}.{self.pick(TLDS)}"
        return e.upper() if upper else e

    def question(self):
        return (self.pick(STEMS).format(t=self.pick(THINGS), v=self.pick(VERBS)))

    def add(self, template, labels, count, make):
        for _ in range(count):
            for _try in range(500):
                text = make()
                assert "\t" not in text and "\n" not in text and len(text) <= 256, text
                assert text.isascii(), text
                if text not in self.seen:
                    break
            else:
                raise SystemExit(f"template {template}: cannot make {count} distinct lines")
            self.seen.add(text)
            self.rows.append((text, labels, template))


def build(seed=SEED):
    g = Gen(seed)
    p = g.pick
    E, P, Q, G, N = "EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"

    g.add("E1", E, 120, lambda: g.email())
    g.add("E2", E, 40, lambda: g.email(upper=True))
    g.add("E3", E, 40, lambda: g.email(sub=True))

    g.add("P1", P, 120, g.phone)
    def p2():
        d = g.phone()
        return d[:5] + " " + d[5:]
    g.add("P2", P, 30, p2)
    g.add("P3", P, 30, lambda: "+91" + g.phone())

    g.add("Q1", Q, 110, lambda: g.question() + "?")
    g.add("Q2", Q, 40, lambda: g.question().lower())
    g.add("Q3", Q, 15, lambda: g.question() + "? ")
    g.add("Q4", Q, 15, lambda: p(ONE_WORD_Q) + "?")

    g.add("G1", G, 70, lambda: p(GREET_LOWER) + p(GREET_TAIL))
    g.add("G2", G, 50, lambda: (lambda s: s[0].upper() + s[1:])(p(GREET_LOWER) + p(GREET_TAIL)))
    g.add("G3", G, 30, lambda: p(["Hello", "Hi", "Hey", "hello", "hi", "hey"]) +
          p([", " + p(FIRST) + "!", "!", "! " + p(GREET_TAIL).strip().capitalize(), ", " + p(FIRST)]))

    g.add("N1", N, 100, lambda: p(FIRST) + " " + p(LAST))
    g.add("N2", N, 25, lambda: (p(FIRST) + " " + p(LAST)).lower())
    def n3():
        form = g.r.randrange(4)
        if form == 0:
            return p(FIRST) + " " + p(FIRST) + " " + p(LAST)
        if form == 1:
            return p(FIRST) + "-" + p(FIRST) + " " + p(LAST)
        if form == 2:
            return p(FIRST) + " " + chr(65 + g.r.randrange(26)) + ". " + p(LAST)
        return p(FIRST) + " O'" + p(LAST)
    g.add("N3", N, 25, n3)

    g.add("M1", f"{G},{E}", 30, lambda: p(["hello", "hi", "hey"]) + " " + g.email())
    g.add("M2", f"{G},{Q}", 30, lambda: p(["hello", "hi", "hey", "good morning", "good afternoon"]) +
          p([",", ""]) + " " + g.question().lower() + "?")
    g.add("M3", f"{E},{P}", 20, lambda: "reach me on " + g.phone() + " or " + g.email())
    g.add("M4", f"{Q},{E}", 15, lambda: "is " + g.email() + " your email?")
    g.add("M5", f"{Q},{P}", 15, lambda: "is " + g.phone() + " your number?")
    g.add("M6", f"{G},{P}", 10, lambda: p(["hey", "hi", "hello"]) + ", call " + g.phone())

    g.add("X1", "NONE", 70, lambda: p(STATEMENTS) + p(STATEMENT_EXTRA))
    def x2():
        f, l, d = p(FIRST).lower(), p(LAST).lower(), p(DOMAINS)
        return p([f"{f}@", f"@{d}.com", f"{f}@{d}", f"{f}.{l}.{d}.com", f"{f}@@{d}.com",
                  f"{f} at {d} dot com", f"{f}@.{d}.com", f"{f}@{d}.c"])
    g.add("X2", "NONE", 40, x2)
    g.add("X3", "NONE", 20, lambda: g.phone() + g.digits(1))
    g.add("X3", "NONE", 10, lambda: g.phone()[:9])
    g.add("X3", "NONE", 25, lambda: str(1 + g.r.randrange(5)) + g.digits(9))
    g.add("X4", "NONE", 30, lambda: p(MID_GREET).format(g=p(["hello", "hi", "hey", "good morning"])))
    g.add("X5", "NONE", 25, lambda: p(TOKENS) if g.r.randrange(2) else p(TOKENS) + " " + p(TOKENS))
    g.add("X6", "NONE", 30, lambda: p(NOT_NAMES))
    g.add("X7", "NONE", 20, lambda: p(["Order", "Ticket", "Invoice", "Shipment", "Case"]) + " " +
          g.digits(4 + g.r.randrange(3)) + p([" shipped.", " is closed.", " was updated.", " arrived."]))

    # NONE lines must really be none of the categories: no question mark, no well-formed phone
    # number or e-mail address, no sentence-initial greeting.
    for text, labels, tmpl in g.rows:
        if labels == "NONE":
            assert "?" not in text, text
            if tmpl not in ("X2", "X3"):
                assert "@" not in text, text
            low = text.lower()
            first = low.replace(",", " ").replace("!", " ").split()[:2]
            assert not (first and (first[0] in ("hello", "hi", "hey") or
                                   " ".join(first) in ("good morning", "good afternoon"))), text
    # shuffle so categories interleave (batch sizes in RB2 take prefixes of this file)
    g.r.shuffle(g.rows)
    return g.rows


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/basepaper/labeled.tsv"
    rows = build()
    lines = [f"# labeled.tsv: generated by tools/basepaper/gen_bp.py, seed {SEED}. "
             "Columns: text, true labels, template (see the script header)."]
    lines += [f"{t}\t{lab}\t{tmpl}" for t, lab, tmpl in rows]
    data = "\n".join(lines) + "\n"
    if out == "-":
        sys.stdout.write(data)
    else:
        with open(out, "w", newline="\n") as f:
            f.write(data)
    by_label = collections.Counter()
    by_cat = collections.Counter()
    by_tmpl = collections.Counter()
    for _, lab, tmpl in rows:
        by_label[lab] += 1
        by_tmpl[tmpl] += 1
        for c in lab.split(","):
            by_cat[c] += 1
    log = sys.stderr
    print(f"gen_bp: {len(rows)} lines, seed {SEED} -> {out}", file=log)
    print("  lines containing each category: " +
          ", ".join(f"{c}={by_cat[c]}" for c in CATS + ["NONE"]), file=log)
    print("  by label set: " + ", ".join(f"{k}={v}" for k, v in sorted(by_label.items())), file=log)
    print("  by template: " + ", ".join(f"{k}={v}" for k, v in sorted(by_tmpl.items())), file=log)


if __name__ == "__main__":
    main()
