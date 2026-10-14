# Reference list for the report (IEEE format), grouped by Literature Survey sub-heading

Built from `literature_review.md` after two verification passes (14 Oct 2026, web search against
publisher, DBLP, ACL Anthology, USENIX and citing papers). Keys (J1…, C1…) match that file, which
also says what each paper contributes. In the report the numbers are assigned in order of first
citation. Where a page range could not be confirmed it is left out rather than guessed.

**Count: 34 papers (22 journal, 12 conference). Per the team's decision only papers are cited: no textbook, web article, patent, monograph, standard or unpublished base paper in the reference list.**

## 2.1 Automata theory and regex compilation

- **J1** M. O. Rabin and D. Scott, "Finite automata and their decision problems," *IBM J. Res. Develop.*, vol. 3, no. 2, pp. 114–125, Apr. 1959, doi: 10.1147/rd.32.0114.
- **J2** R. McNaughton and H. Yamada, "Regular expressions and state graphs for automata," *IRE Trans. Electron. Comput.*, vol. EC-9, no. 1, pp. 39–47, Mar. 1960, doi: 10.1109/TEC.1960.5221603.
- **J3** K. Thompson, "Programming techniques: Regular expression search algorithm," *Commun. ACM*, vol. 11, no. 6, pp. 419–422, Jun. 1968, doi: 10.1145/363347.363387.
- **J4** A. V. Aho and M. J. Corasick, "Efficient string matching: An aid to bibliographic search," *Commun. ACM*, vol. 18, no. 6, pp. 333–340, Jun. 1975, doi: 10.1145/360825.360855.

## 2.2 Multi-pattern matching, DFA state explosion and compression

- **J5** C. Xu, S. Chen, J. Su, S. M. Yiu, and L. C. K. Hui, "A survey on regular expression matching for deep packet inspection: Applications, algorithms, and hardware platforms," *IEEE Commun. Surveys Tuts.*, vol. 18, no. 4, pp. 2991–3029, 2016, doi: 10.1109/COMST.2016.2565660.
- **J6** D. Ficara, A. Di Pietro, S. Giordano, G. Procissi, F. Vitucci, and G. Antichi, "Differential encoding of DFAs for fast regular expression matching," *IEEE/ACM Trans. Netw.*, vol. 19, no. 3, pp. 683–694, Jun. 2011, doi: 10.1109/TNET.2010.2089639.
- **J7** M. Becchi and P. Crowley, "A-DFA: A time- and space-efficient DFA compression algorithm for fast regular expression evaluation," *ACM Trans. Archit. Code Optim.*, vol. 10, no. 1, Art. no. 4, 2013. *(article page not re-confirmed online; cited by later work)*
- **J8** A. X. Liu and E. Norige, "A de-compositional approach to regular expression matching for network security," *IEEE/ACM Trans. Netw.*, vol. 27, no. 6, pp. 2179–2191, Dec. 2019, doi: 10.1109/TNET.2019.2941920.
- **J9** S. Dharmapurikar and J. W. Lockwood, "Fast and scalable pattern matching for network intrusion detection systems," *IEEE J. Sel. Areas Commun.*, vol. 24, no. 10, pp. 1781–1792, Oct. 2006.
- **C2** S. Kumar, S. Dharmapurikar, F. Yu, P. Crowley, and J. Turner, "Algorithms to accelerate multiple regular expressions matching for deep packet inspection," in *Proc. ACM SIGCOMM*, 2006, pp. 339–350, doi: 10.1145/1159913.1159952.
- **C3** M. Becchi and P. Crowley, "A hybrid finite automaton for practical deep packet inspection," in *Proc. ACM CoNEXT*, 2007, pp. 1–12.
- **C4** M. Becchi and P. Crowley, "Extending finite automata to efficiently match Perl-compatible regular expressions," in *Proc. ACM CoNEXT*, 2008, doi: 10.1145/1544012.1544037.
- **C5** X. Wang *et al.*, "Hyperscan: A fast multi-pattern regex matcher for modern CPUs," in *Proc. 16th USENIX Symp. Netw. Syst. Design Implement. (NSDI)*, 2019, pp. 631–648.
- **C6** J. C. Davis, C. A. Coghlan, F. Servant, and D. Lee, "The impact of regular expression denial of service (ReDoS) in practice: An empirical study at the ecosystem scale," in *Proc. ESEC/FSE*, 2018, pp. 246–256, doi: 10.1145/3236024.3236027.

## 2.3 Parallel pattern matching and concurrency

- **J10** D. Luchaup, R. Smith, C. Estan, and S. Jha, "Speculative parallel pattern matching," *IEEE Trans. Inf. Forensics Security*, vol. 6, no. 2, pp. 438–451, Jun. 2011, doi: 10.1109/TIFS.2011.2112647.
- **J11** Y. Ko, M. Jung, Y.-S. Han, and B. Burgstaller, "A speculative parallel DFA membership test for multicore, SIMD and cloud computing environments," *Int. J. Parallel Program.*, vol. 42, pp. 456–489, 2014, doi: 10.1007/s10766-013-0258-5.
- **J12** Y.-H. E. Yang and V. K. Prasanna, "Robust and scalable string pattern matching for deep packet inspection on multicore processors," *IEEE Trans. Parallel Distrib. Syst.*, vol. 24, pp. 2283–2292, 2013, doi: 10.1109/TPDS.2012.217.
- **C1** T. Mytkowicz, M. Musuvathi, and W. Schulte, "Data-parallel finite-state machines," in *Proc. ASPLOS*, 2014, pp. 529–542, doi: 10.1145/2541940.2541988.
- **J22** J. L. Gustafson, "Reevaluating Amdahl's law," *Commun. ACM*, vol. 31, no. 5, pp. 532–533, May 1988, doi: 10.1145/42411.42415.
- **J23** L. Lamport, "Specifying concurrent program modules," *ACM Trans. Program. Lang. Syst.*, vol. 5, no. 2, pp. 190–222, Apr. 1983.
- **C10** A. Baumann, J. Appavoo, O. Krieger, and T. Roscoe, "A fork() in the road," in *Proc. Workshop Hot Topics Oper. Syst. (HotOS)*, 2019, doi: 10.1145/3317550.3321435.

## 2.4 Log analysis, datasets and sensitive-data detection

- **J14** S. He, P. He, Z. Chen, T. Yang, Y. Su, and M. R. Lyu, "A survey on automated log analysis for reliability engineering," *ACM Comput. Surv.*, vol. 54, no. 6, Art. no. 130, 2021, doi: 10.1145/3460345.
- **J15** P. He, J. Zhu, S. He, J. Li, and M. R. Lyu, "Towards automated log parsing for large-scale log data analysis," *IEEE Trans. Dependable Secure Comput.*, 2018. *(volume/pages not confirmed)*
- **J16** M. Landauer *et al.*, "System log clustering approaches for cyber security applications: A survey," *Comput. Secur.*, vol. 92, Art. no. 101739, 2020, doi: 10.1016/j.cose.2020.101739.
- **J17** M. Landauer, F. Skopik, M. Frank, W. Hotwagner, M. Wurzenberger, and A. Rauber, "Maintainable log datasets for evaluation of intrusion detection systems," *IEEE Trans. Dependable Secure Comput.*, vol. 20, no. 4, pp. 3466–3482, 2023.
- **J18** M. Landauer, F. Skopik, M. Wurzenberger, W. Hotwagner, and A. Rauber, "Have it your way: Generating customized log datasets with a model-driven simulation testbed," *IEEE Trans. Rel.*, vol. 70, no. 1, pp. 402–415, Mar. 2021.
- **J19** M. Dayarathna and S. Perera, "Recent advancements in event processing," *ACM Comput. Surv.*, vol. 51, no. 2, pp. 33:1–33:36, 2018, doi: 10.1145/3170432.
- **C7** P. He, J. Zhu, Z. Zheng, and M. R. Lyu, "Drain: An online log parsing approach with fixed depth tree," in *Proc. IEEE Int. Conf. Web Services (ICWS)*, 2017, doi: 10.1109/ICWS.2017.13.
- **C8** J. Zhu, S. He, P. He, J. Liu, and M. R. Lyu, "Loghub: A large collection of system log datasets for AI-driven log analytics," in *Proc. IEEE Int. Symp. Softw. Rel. Eng. (ISSRE)*, 2023.
- **J20** I. Pilán, P. Lison, L. Øvrelid, A. Papadopoulou, D. Sánchez, and M. Batet, "The Text Anonymization Benchmark (TAB): A dedicated corpus and evaluation framework for text anonymization," *Comput. Linguist.*, vol. 48, no. 4, pp. 1053–1101, Dec. 2022, doi: 10.1162/coli_a_00458.
- **C9** P. Lison, I. Pilán, D. Sánchez, M. Batet, and L. Øvrelid, "Anonymisation models for text data: State of the art, challenges and future directions," in *Proc. 59th Annu. Meeting Assoc. Comput. Linguistics (ACL-IJCNLP)*, 2021, pp. 4188–4203, doi: 10.18653/v1/2021.acl-long.323.
- **J21** H. P. Gumm, "A new class of check-digit methods for arbitrary number systems," *IEEE Trans. Inf. Theory*, vol. 31, no. 1, pp. 102–105, Jan. 1985, doi: 10.1109/TIT.1985.1056991.

## 2.5 Benchmarking methodology

- **C11** T. Hoefler and R. Belli, "Scientific benchmarking of parallel computing systems: Twelve ways to tell the masses when reporting performance results," in *Proc. Int. Conf. High Perform. Comput., Netw., Storage Anal. (SC)*, 2015, Art. no. 73, doi: 10.1145/2807591.2807644.
- **C12** A. Roodt, B. K. M. Watling, W. Bester, B. van der Merwe, S. Sung, and Y.-S. Han, "Benchmarking regular expression matching," in *Implementation and Application of Automata (CIAA 2024)* (Lecture Notes in Computer Science, vol. 15015). Cham, Switzerland: Springer, 2024.

## Corrections made during verification (for the record)

- J8: the second author is E. Norige, not Torng.
- J13 ("Picking pesky parameters…", IEEE TPDS 2016): no such paper found; removed.
- Details not confirmed online are omitted from the entries above: J15 volume/pages, C7 and C8 and C10 page ranges, C12 start page.
