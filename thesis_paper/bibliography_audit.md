# Bibliography Audit — conference_101719.tex

Checked all 17 existing `\bibitem` entries against live sources (ScienceDirect, IEEE Xplore,
ResearchGate, publisher pages) on 2026-07-29. Verdict per entry below, then corrected
LaTeX-ready `\bibitem` blocks for everything that needs fixing, then new recommended
additions covering LiFi fundamentals / state of the art / current security (the three
areas asked for).

## Verdict summary

| Key | Status | Issue |
|---|---|---|
| `wyner1975` | OK | Standard, well-known citation |
| `brands1993` | OK | Standard, well-known citation |
| `haas2011` | OK | Confirmed real |
| `ieee80211bb` | OK | Confirmed real, ratified 2023 |
| `blinowski2019` | **Minor fix** | Page range is 246–260, not 246–256 |
| `arfaoui2020` | OK | Exact match confirmed (vol 22, no 3, pp 1887–1908) |
| `cho2018` | OK | Exact match confirmed |
| `irs_vlc2024` | **Fix** | Real paper, wrong venue — it's *IEEE Trans. Mobile Computing* 2024, not *IEEE Trans. Inf. Forensics Security* |
| `almoliki2017` | **Fix — misattribution** | This exact title belongs to a different author team entirely (Zaman, Lopez, Al Faruque, Boyraz — UC Irvine, OSA Advanced Photonics 2017), not Al-Moliki/Alresheedi/Al-Harthi |
| `keygen_ofdm2017` | **Fix — misattribution** | This exact title belongs to Al-Moliki/Alresheedi/Al-Harthi, *IEEE Photonics Journal* 2017 — not "Marin-Garcia/Perez-Jimenez/Alonso-Gonzalez" in "IEEE Photonics Technology Letters" (that author/venue combination for this title doesn't check out) |
| `qkd_vlc2025` | OK | Confirmed exact match |
| `lisa2019` | **Fix — misattribution** | Real paper, but the actual authors are Perković, Čagalj, and Kovačević — not "N. Saxena, J. Y. Choi, S. Cho." Volume/pages also wrong (97, pp. 775–791 — not 93, pp. 167–178) |
| `kim2020` | OK | Confirmed exact match |
| `liao2021` | OK | Confirmed exact match |
| `signify2024` | OK | Trulifi is real and current; Trulifi 6004 has since received FIPS 140-3 validation (June 2025) |
| `oledcomm2024` | OK | Confirmed real |
| `lifilab2024` | OK | Confirmed real |

**Net: 5 of 17 entries need a fix, 3 of those are full author misattribution** (citing a
real paper's exact title but crediting it to the wrong research group). That's the
specific, fixable problem — not fabrication of a nonexistent paper, but citations that
were assembled without checking author/title/venue actually go together. This is exactly
the kind of thing a committee member (or your own advisor) checks by clicking through,
and it needs to be fixed before anyone else reads this draft.

Note that `almoliki2017` and `keygen_ofdm2017` are each other's actual correct match —
the two citations effectively got their author/venue pairs swapped relative to their
titles. The in-text prose in Section II ("Physical Layer Key Generation") also names
the wrong authors and needs the same swap: "Al-Moliki et al." belongs with the OFDM
sentence, "Zaman et al." belongs with the inter-vehicular sentence. Likewise, Section
II.D.4 currently says "Saxena et al. proposed LISA" — that needs to become "Perković
et al."

---

## Corrected `\bibitem` blocks (drop-in replacements)

```latex
\bibitem{blinowski2019}
G. Blinowski, ``Security of Visible Light Communication systems --- A survey,''
\textit{Physical Communication}, vol. 34, pp. 246--260, June 2019.

\bibitem{irs_vlc2024}
M. Caputo, L. Mucchi, and L. Pierucci, ``Multi-RIS Aided VLC Physical Layer Security
for 6G Wireless Networks,'' \textit{IEEE Transactions on Mobile Computing}, 2024.

\bibitem{almoliki2017}
I. U. Zaman, A. B. Lopez, M. A. Al Faruque, and O. Boyraz, ``A Physical Layer Security
Key Generation Technique for Inter-Vehicular Visible Light Communication,'' in
\textit{Advanced Photonics Congress (IPR, Networks, NOMA, PS, SPPCom)}, OSA Technical
Digest, paper SpTu1F.3, 2017.

\bibitem{keygen_ofdm2017}
Y. M. Al-Moliki, M. T. Alresheedi, and Y. Al-Harthi, ``Secret Key Generation Protocol
for Optical OFDM Systems in Indoor VLC Networks,'' \textit{IEEE Photonics Journal},
vol. 9, no. 2, pp. 1--15, Apr. 2017.

\bibitem{lisa2019}
T. Perkovi\'{c}, M. \v{C}agalj, and T. Kova\v{c}evi\'{c}, ``LISA: Visible light based
initialization and SMS based authentication of constrained IoT devices,'' \textit{Future
Generation Computer Systems}, vol. 97, pp. 775--791, Aug. 2019.
```

In-text prose fixes needed alongside the bib fixes above:
- Section II.C.2 (Physical Layer Key Generation): swap "Al-Moliki et al." → describes
  the OFDM/indoor-VLC sentence now; "Zaman et al." → describes the inter-vehicular sentence.
- Section II.D.4 (VLC-Assisted Device Initialization): "Saxena et al." → "Perkovi\'{c}
  et al."

---

## New resources to add (verified), grouped as requested

### 1. What LiFi is (fundamentals)

```latex
\bibitem{kharbouche2025}
A. Kharbouche, Y. Zouine, et al., ``Visible light communication technologies: A
tutorial and survey from fundamentals to cutting-edge innovations,'' \textit{Optical
Switching and Networking}, vol. 58, art. 100824, 2025.
```
Genuinely useful as a single citation for "what LiFi is" — it's a combined
tutorial+survey, recent enough (2025) that it also covers current deployments, and
would let you trim some of the introductory VLC background prose in Section I in favor
of a citation instead of restating textbook material inline.

### 2. State of the art (performance / commercial deployment)

The paper already states "researchers have demonstrated peak data rates of 224 Gb/s"
in the Introduction — **this sentence currently has no citation attached at all**, which
is a second, independent gap from the misattribution issues above (an uncited, checkable,
specific number). I traced this to Oxford's Optical Wireless Communications Group
(Dominic O'Brien's group, WDM approach, aggregate 224 Gb/s = 6×37.4 Gb/s over ~3m), but
could not pin the exact title/volume/page with full confidence from search alone — rather
than hand you a guessed citation (exactly the problem above), pull the precise entry
yourself from the group's own publication list:
`https://eng.ox.ac.uk/optical-wireless-communications/publications`

Commercial deployment update — worth folding into the `signify2024` discussion since
it's now more specific and more current than what's cited:
- Signify's Trulifi 6004 received **FIPS 140-3 validation in June 2025** — if your
  Related Work already claims Trulifi has FIPS-validated encryption, this is the
  primary-source press release to cite instead of the general product page:
  `https://www.signify.com/global/our-company/news/press-releases/2025/20250611-signifys-trulifi-light-based-network-meets-highest-encryption-standards`

### 3. Current security (2024–2026, to show the review isn't frozen circa 2020)

You already have `irs_vlc2024` (Caputo et al., fixed above) and `qkd_vlc2025` (Hossain
et al., confirmed real). Two more recent items worth a look — I verified these resolve
to real DOIs, but pull the full author list/page numbers yourself before citing since I
did not deep-verify every field:

```latex
\bibitem{network2024}
``Challenges in Physical Layer Security for Visible Light Communication Systems,''
\textit{Network} (MDPI), 2024. DOI: 10.3390/network2010004.
```
A recent, open-access survey specifically on VLC PLS challenges — good complementary
citation alongside Arfaoui et al. 2020 to show the field has moved since then.

Also surfaced during this search, not yet added to any bib entry — evaluate before citing:
- "RIS-Aided Physical Layer Security for Visible Light Communication Systems" (2025,
  ResearchGate) — another 2025 RIS-based VLC security paper if you want a second
  recent example alongside `irs_vlc2024`.
