#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CC834
# SPDX-License-Identifier: GPL-3.0-only
"""Regenerate ranked completion data with wordfreq==3.1.1 (build-time only)."""
from pathlib import Path
from wordfreq import iter_wordlist

destination = Path(__file__).resolve().parents[1] / "src/suggestions"
destination.mkdir(parents=True, exist_ok=True)
for language in ("en", "sv"):
    words = []
    for word in iter_wordlist(language):
        if 2 <= len(word) <= 40 and word[0].isalpha() and all(c.isalpha() or c == "'" for c in word):
            words.append(word)
        if len(words) == 40000:
            break
    (destination / f"{language}.txt").write_text("\n".join(words) + "\n", encoding="utf-8")
    print(language, len(words))
