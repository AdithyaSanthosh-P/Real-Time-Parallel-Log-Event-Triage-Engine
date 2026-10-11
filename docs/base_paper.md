# Parallel Text Classification System Using Regex Pattern Matching on Multi-Core Architectures

*Case Study — base paper provided by the course professor (Compiler Design). Converted from `base_paper.docx`. Images are described in text; decorative images omitted.*

## Abstract

This research presents a high-performance parallel text classification system that leverages multi-core architectures to overcome the performance limitations of traditional sequential text processing. The system implements process-level parallelism to concurrently evaluate multiple regular expression patterns across available CPU cores, enabling real-time classification of diverse text categories including email addresses, phone numbers, personal names, questions, and greetings.

Experimental results demonstrate significant performance improvements, with the parallel implementation achieving 2.4× speedup over sequential approaches while maintaining 96.3% classification accuracy. The system processes text inputs in 3.7 milliseconds compared to 8.9 milliseconds for sequential processing, representing a 140% improvement in throughput. Comprehensive testing across 1,250 text samples validates the system's reliability and scalability, with near-linear performance scaling up to 6 processor cores. The architecture employs a modular three-layer design following compiler principles, with separate components for input processing, parallel pattern matching, and output generation. This research addresses critical gaps in current text processing systems by effectively utilizing modern multi-core hardware while providing robust conflict resolution and dynamic load balancing capabilities suitable for production deployment in real-time applications.

## 1. Introduction

### 1.1 Background and Motivation

The exponential growth of digital text data in contemporary computing environments has created unprecedented demands for efficient text processing and classification systems. From user registration forms and contact information extraction to chatbot interactions and content moderation, the ability to quickly and accurately categorize text inputs has become fundamental to modern software applications. Traditional sequential text processing methods, while reliable, increasingly struggle to meet the performance requirements of real-time systems and large-scale data processing pipelines.

The evolution of multi-core processor architectures has transformed the computational landscape, with modern systems typically featuring 4 to 16 processing cores in standard configurations. However, conventional text processing algorithms have been slow to adapt to these architectural advancements, continuing to operate sequentially and consequently underutilizing available computational resources. This architectural mismatch represents a significant opportunity for performance optimization through parallelization.

Regular expressions (regex) have emerged as the de facto standard for pattern matching in text processing due to their expressiveness, flexibility, and widespread library support. The combination of regex pattern matching with parallel computing principles offers a promising approach to addressing the performance challenges in modern text classification systems.

### 1.2 Problem Statement

The core research problem addressed in this case study revolves around the performance limitations of sequential text classification systems in multi-core computing environments. Comprehensive analysis of existing text processing solutions reveals several critical challenges:

Current systems typically process regex patterns sequentially, examining one pattern at a time against input text. This approach becomes increasingly inefficient as the number and complexity of patterns grow, leading to substantial processing delays for comprehensive text classification tasks. The sequential nature of these systems fails to leverage the parallel processing capabilities inherent in modern multi-core architectures.

Additionally, traditional implementations often lack sophisticated conflict resolution mechanisms when multiple patterns match the same input text. This can lead to ambiguous classifications or require complex priority-based resolution logic that further impacts performance. The absence of dynamic load balancing in pattern matching distribution results in suboptimal resource utilization and inconsistent performance across different input types.

### 1.3 Research Objectives

- Design and implement a multi-layer architecture that enables concurrent evaluation of multiple regex patterns across available processor cores while maintaining classification accuracy and system reliability.
- Develop sophisticated pattern matching algorithms that efficiently categorize diverse text types including structured data formats (emails, phone numbers), natural language constructs (questions, greetings), and semantic content (names, dates).
- Create a dynamic task allocation system that optimizes pattern distribution based on computational complexity, historical performance data, and current system load conditions.
- Implement robust result aggregation and conflict resolution mechanisms that ensure consistent and accurate text classification across parallel processing units.
- Empirically validate system performance through comprehensive testing across various text categories, input complexities, and hardware configurations.

### 1.4 Project Scope and Limitations

The scope encompasses a parallel text classification system for common text patterns encountered in user input processing and data validation. The implementation focuses on seven primary text categories: email addresses, phone numbers, personal names, questions, greetings, dates, and URLs.

The system employs process-level parallelism using standard POSIX system calls and utilizes the POSIX regex library for pattern matching operations. Performance analysis concentrates on multi-core systems with 2–8 processing cores, with comparative evaluation against established sequential text classification approaches.

Intentional limitations: English-language text patterns only (architecture supports extension); standalone text classification without integration into larger NLP pipelines; CPU-based parallelism only, no GPU or specialized hardware.

## 2. Related Work

### 2.1 Historical Development of Pattern Matching

The theoretical foundations of pattern matching trace back to formal language theory and automata theory in the mid-20th century. Stephen Kleene's development of regular expressions in the 1950s established the mathematical basis for describing text patterns, while Ken Thompson's implementation of regex in the QED text editor in the 1960s demonstrated their practical utility.

Regex implementations progressed from basic Thompson NFA-based matchers to modern backtracking implementations with extended features. Henry Spencer's regex library in the 1980s and subsequent POSIX standardization established consistent behavior across platforms, enabling widespread adoption in Unix-like systems.

Integration into Perl, Python, and Java introduced lazy quantifiers, lookahead assertions, and capture groups, significantly expanding expressive power.

### 2.2 Regex in Text Processing

Regular expressions are ubiquitous due to compact syntax and powerful matching. Research has shown well-designed regex patterns can achieve performance comparable to custom-built parsing engines for many common tasks, and analysis of regex optimization techniques has informed efficient pattern compilation and matching strategies.

Studies of regex in data validation systems identified common performance bottlenecks in complex pattern matching and proposed optimizations for high-throughput validation. Recent work on industrial-scale pipelines found pattern matching accounts for significant computational overhead in data-intensive applications, emphasizing the need for optimized engines and parallel approaches.

### 2.3 Parallel Computing in NLP

Document classification tasks can be parallelized with near-linear speedup on multi-core systems, establishing principles for task decomposition and load balancing. Studies of thread-level parallelism in regex matching achieved improvements through work-stealing algorithms and cache-aware data structures, and highlighted the challenges of parallelizing state-based pattern matching. Surveys identify pattern matching as a promising candidate for parallel optimization due to the independent nature of many matching operations.

### 2.4 Research Gap Analysis

1. Most existing regex implementations operate sequentially, processing patterns one at a time, failing to leverage multi-core systems for tasks requiring evaluation of many independent patterns.
2. Parallel text processing research focuses on document-level or pipeline-stage parallelism, with limited attention to parallel pattern matching *within* individual classification tasks.
3. Existing conflict resolution often uses simplistic priority or first-match policies; approaches considering pattern specificity, match quality, and context are needed.
4. Limited research on dynamic load balancing tailored to regex workloads, where matching time varies significantly across inputs and pattern types.

## 3. System Design

### 3.1 Architecture Diagram

*(Image, described.)* Three-layer architecture:

- **Input Layer**
  - *Text Normalization*: case normalization, whitespace standardization, special character handling.
  - *Pattern Queue Management*: dynamic task allocation, load prediction algorithms, priority-based scheduling.
- **Parallel Processing Layer** — one matcher per core, outputs merged:
  - Email Matcher (Core 0), Phone Matcher (Core 1), Question (Core 2), Greeting Matcher (Core 3), Name (Core 4), "…" (more).
- **Output Layer**
  - *Result Aggregation*: conflict resolution algorithms, pattern specificity analysis, match quality scoring.
  - *Confidence Calculation*: probabilistic accuracy assessment, historical performance data, match completeness evaluation.
  - → Final Output (with confidence level, e.g. HIGH).

```
Input Layer:     [Text Normalization]  [Pattern Queue Management]
                              \            /
Parallel Layer:  [Email C0] [Phone C1] [Question C2] [Greeting C3] [Name C4] ...
                              \   MERGE   /
Output Layer:    [Result Aggregation] --> [Confidence Calculation] --> Final Output
```

### 3.2 Input Processing Module

The Input Processing Module is the front-end, preparing raw text for parallel matching. **Text normalization** converts to lowercase, removes extra whitespace, and handles special characters consistently so matching behaves predictably regardless of formatting.

**Input validation** checks length constraints, character encoding compatibility, and basic syntactic validity before matching, with error handling and feedback for malformed inputs. **Queue management** coordinates distribution of matching tasks across cores, using priority-based scheduling that considers pattern complexity and historical performance data.

```text
MODULE InputProcessing
  CONSTANT MAX_INPUT_LENGTH = 256
  CONSTANT ALLOWED_CHARS = "a-zA-Z0-9@.-_?! "

  PROCEDURE process_input(raw_text)
    // Step 1: Validate input length
    IF LENGTH(raw_text) > MAX_INPUT_LENGTH THEN
        RETURN ERROR "Input exceeds maximum length"
    END IF

    // Step 2: Validate character set
    IF NOT contains_only_allowed_chars(raw_text, ALLOWED_CHARS) THEN
        RETURN ERROR "Input contains invalid characters"
    END IF

    // Step 3: Normalize text
    normalized_text = convert_to_lowercase(raw_text)
    normalized_text = trim_whitespace(normalized_text)
    normalized_text = normalize_special_chars(normalized_text)

    // Step 4: Add to processing queue
    task_id = add_to_processing_queue(normalized_text)

    RETURN task_id
  END PROCEDURE

  PROCEDURE normalize_special_chars(text)
    REPLACE_ALL "  +" WITH " " IN text    // Multiple spaces to single
    REPLACE_ALL "\t+" WITH " " IN text    // Tabs to spaces
    REPLACE_ALL "\n+" WITH "" IN text     // Remove newlines
    RETURN text
  END PROCEDURE
END MODULE
```

### 3.3 Parallel Pattern Matching Module

The core of the system: process-level parallelism to evaluate regex patterns concurrently. Master-worker architecture — the main process distributes patterns and collects results; worker processes perform matching. Each worker specializes in one category (email, phone, name, etc.).

Each matcher process is bound to a specific CPU (core affinity) to minimize cache invalidation and context switching. Dynamic load monitoring can redistribute patterns on imbalance. Process isolation means one matcher's failure doesn't affect others; heartbeat monitoring and automatic restart maintain continuous operation.

```text
MODULE ParallelPatternMatching
  // Pattern database initialization
  PATTERNS = [
    {category: "EMAIL",    regex: "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}"},
    {category: "PHONE",    regex: "[6-9][0-9]{9}"},
    {category: "QUESTION", regex: ".*\\?$"},
    {category: "GREETING", regex: "^(hello|hi|hey|good morning|good afternoon)"},
    {category: "NAME",     regex: "^[A-Z][a-z]+ [A-Z][a-z]+$"}
  ]

  PROCEDURE parallel_classify(normalized_text)
    child_processes = []
    results_pipe = create_communication_pipe()

    // Create child processes for each pattern
    FOR i = 0 TO LENGTH(PATTERNS) - 1 DO
        pid = fork()

        IF pid == 0 THEN
            // Child process - specialized pattern matching
            set_cpu_affinity(i % available_cores)
            match_result = execute_pattern_match(normalized_text, PATTERNS[i])
            send_result(results_pipe, match_result)
            EXIT(0)
        ELSE IF pid > 0 THEN
            child_processes.append(pid)
        ELSE
            LOG_ERROR "Failed to create process for pattern: " + PATTERNS[i].category
        END IF
    END FOR

    // Collect results from all children
    classification_results = []
    FOR EACH child_pid IN child_processes DO
        result = receive_result(results_pipe)
        classification_results.append(result)
        waitpid(child_pid)  // Clean up child process
    END FOR

    RETURN classification_results
  END PROCEDURE

  PROCEDURE execute_pattern_match(text, pattern)
    compiled_regex = regcomp(pattern.regex)
    IF compiled_regex == NULL THEN
        RETURN {category: pattern.category, matched: false, error: "Compilation failed"}
    END IF

    match_result = regexec(compiled_regex, text)
    regfree(compiled_regex)

    RETURN {
        category: pattern.category,
        matched: (match_result == 0),
        pattern: pattern.regex,
        timestamp: current_time()
    }
  END PROCEDURE
END MODULE
```

### 3.4 Output Generation Module

Synthesizes results from the parallel matchers. **Conflict resolution** considers pattern specificity, match completeness, and historical accuracy when multiple patterns match.

**Confidence calculation** produces probabilistic assessments based on match completeness, pattern complexity, and historical performance; per-pattern accuracy statistics weight decisions. High-confidence predictions (≥90%) achieve 99.2% accuracy in validation testing.

Supports multiple output formats: structured JSON (APIs), human-readable text (UIs), compact binary (high-throughput). Includes logging and analytics.

```text
MODULE OutputGeneration
  // Confidence thresholds
  CONSTANT HIGH_CONFIDENCE = 0.9
  CONSTANT MEDIUM_CONFIDENCE = 0.7
  CONSTANT LOW_CONFIDENCE = 0.5

  PROCEDURE generate_output(classification_results)
    // Filter only successful matches
    positive_matches = FILTER classification_results WHERE matched == true

    IF LENGTH(positive_matches) == 0 THEN
        RETURN generate_no_match_output()
    ELSE IF LENGTH(positive_matches) == 1 THEN
        RETURN generate_single_match_output(positive_matches[0])
    ELSE
        RETURN generate_multi_match_output(positive_matches)
    END IF
  END PROCEDURE

  PROCEDURE generate_multi_match_output(matches)
    // Conflict resolution based on multiple factors
    resolved_match = resolve_conflicts(matches)
    confidence = calculate_confidence(resolved_match, matches)

    RETURN {
        primary_classification: resolved_match.category,
        confidence_score: confidence,
        all_matches: matches,
        conflict_resolved: true,
        timestamp: current_time()
    }
  END PROCEDURE

  PROCEDURE resolve_conflicts(matches)
    // Factor 1: Pattern specificity (more specific patterns win)
    specificity_scores = CALCULATE_SPECIFICITY(matches)

    // Factor 2: Match completeness (longer matches preferred)
    completeness_scores = CALCULATE_COMPLETENESS(matches)

    // Factor 3: Historical accuracy
    accuracy_scores = GET_HISTORICAL_ACCURACY([m.category FOR m IN matches])

    // Weighted scoring
    total_scores = []
    FOR i = 0 TO LENGTH(matches) - 1 DO
        score = (specificity_scores[i] * 0.4) +
                (completeness_scores[i] * 0.3) +
                (accuracy_scores[i] * 0.3)
        total_scores.append(score)
    END FOR

    best_match_index = ARGMAX(total_scores)
    RETURN matches[best_match_index]
  END PROCEDURE

  PROCEDURE calculate_confidence(primary_match, all_matches)
    base_confidence = GET_BASE_CONFIDENCE(primary_match.category)

    // Adjust based on match quality
    IF primary_match.matched_text == EXACT_MATCH THEN
        base_confidence = base_confidence * 1.2
    END IF

    // Adjust based on conflict severity
    IF LENGTH(all_matches) > 1 THEN
        conflict_penalty = 0.9 ^ (LENGTH(all_matches) - 1)
        base_confidence = base_confidence * conflict_penalty
    END IF

    RETURN CLAMP(base_confidence, 0.0, 1.0)
  END PROCEDURE
END MODULE
```

## 4. Results and Implementation

### 4.1 Performance Benchmarks

**Experimental setup:** Intel Core i7-10750H (6 physical cores), 16 GB DDR4 RAM. 1,250 text samples across all supported categories, balanced across pattern types and complexity levels.

**Execution time analysis:**

| Test Case | Input Text | Sequential (ms) | Parallel (ms) | Speedup |
|---|---|---|---|---|
| Email Validation | `"john@example.com"` | 8.9 | 3.7 | 2.41× |
| Phone Detection | `"9876543210"` | 7.2 | 2.9 | 2.48× |
| Question Detection | `"What is your name?"` | 6.8 | 2.8 | 2.43× |
| Greeting Detection | `"hello everyone"` | 6.5 | 2.7 | 2.41× |
| Name Recognition | `"John Smith"` | 7.1 | 3.0 | 2.37× |
| **Average** | | **7.3** | **3.0** | **2.43×** |

**Inference:** Average speedup of 2.43× over sequential processing, consistent across input complexities; complex patterns (email with additional heuristic checks) reach 2.5–2.8×. Variation reflects differences in parallelization efficiency across pattern types and the fixed overhead of process management.

### 4.2 Accuracy Measurements

| Pattern Type | Test Cases | Correct | Incorrect | Accuracy | Precision | Recall |
|---|---|---|---|---|---|---|
| Email | 250 | 247 | 3 | 98.8% | 99.2% | 98.4% |
| Phone | 200 | 198 | 2 | 99.0% | 98.5% | 99.5% |
| Question | 180 | 172 | 8 | 95.6% | 96.1% | 95.0% |
| Greeting | 150 | 144 | 6 | 96.0% | 95.4% | 96.7% |
| Name | 120 | 115 | 5 | 95.8% | 96.6% | 94.9% |
| **Overall** | **900** | **876** | **24** | **97.3%** | **97.2%** | **97.3%** |

**Inference:** Overall accuracy 97.3%. Structured formats (email, phone) reach 98.7% due to well-defined patterns; natural language constructs (questions, greetings) reach 93.2%, reflecting inherent ambiguity. High-precision patterns (email at 99.1%) minimize false positives, suiting validation; high-recall patterns (greeting at 95.8%) minimize false negatives, suiting conversational systems.

### 4.3 Scalability Analysis

| Cores | Execution Time (ms) | Speedup | Efficiency | CPU Utilization |
|---|---|---|---|---|
| 1 | 7.3 | 1.00× | 100% | 98% |
| 2 | 3.8 | 1.92× | 96% | 95% |
| 4 | 2.1 | 3.48× | 87% | 92% |
| 6 | 1.5 | 4.87× | 81% | 89% |
| 8 | 1.3 | 5.62× | 70% | 85% |

**Inference:** Near-linear scaling for appropriately sized workloads (1.92× at 2 cores, 3.48× at 4, 4.87× at 6) until system-specific limits. Memory-intensive patterns hit diminishing returns earlier (memory bandwidth); compute-intensive patterns scale further. Dynamic scheduling kept core utilization variance within 8% during sustained operation.

### 4.4 Module-wise Input/Output

**Input Processing Module tests:**

| Input | Normalized Output | Status |
|---|---|---|
| `"  Hello   World  "` | `"hello world"` | ✅ PASS |
| `"TEST@EXAMPLE.COM"` | `"test@example.com"` | ✅ PASS |
| `"What's your name?"` | `"what's your name?"` | ✅ PASS |
| `""` | Error: Empty input | ✅ PASS |
| 300-character string | Error: Input too long | ✅ PASS |

**Parallel Pattern Matching Module tests:**

| Input | Detected Patterns | Cores Used |
|---|---|---|
| `"john@company.com"` | EMAIL | Core 0 |
| `"9876543210"` | PHONE | Core 1 |
| `"What is this?"` | QUESTION | Core 2 |
| `"hello there"` | GREETING | Core 3 |
| `"John Smith"` | NAME | Core 4 |
| `"hello john@test.com"` | GREETING, EMAIL | Cores 3, 0 |

**Output Generation Module tests:**

| Input Patterns (detected by cores) | Resolved Output | Confidence |
|---|---|---|
| [EMAIL] | EMAIL | 98% |
| [QUESTION, GREETING] | GREETING | 87% |
| [EMAIL, NAME] | EMAIL | 92% |
| [PHONE, QUESTION] | QUESTION | 85% |
| [] | NO_MATCH | 100% |

**Inference:** All modules function robustly. Input module handles diverse formats and edge cases; matching module distributes work across cores and identifies multiple matches; output module's confidence scores correlate strongly with accuracy (≥90% confidence → 99.2% accuracy).

## 5. Conclusion

### 5.1 Research Summary

The research demonstrates the feasibility and benefits of parallel regex-based text classification on multi-core systems, with ~2.4× speedup across diverse test cases while maintaining high accuracy. Claimed contributions: a dynamic task allocation algorithm for load balancing pattern-matching workloads; a multi-factor conflict resolution system for ambiguous classifications; and an overall architecture serving as a reference model for parallelizing similar pattern recognition tasks.

### 5.2 Practical Implications

- Web applications: real-time form validation with faster feedback.
- Data pipelines: accelerated text categorization in ETL and analytics.
- Chatbots/dialog systems: rapid intent recognition.
- Content moderation: faster identification of structured information in user content.

The approach offers a migration path for existing systems to use modern hardware without full redesign; the modular architecture allows parallelizing specific categories incrementally.

### 5.3 Limitations and Future Work

- Optimal performance with input batches above ~10 texts; smaller batches see reduced speedup due to fixed parallelization overhead — better suited to batch processing than single-text classification.
- Limited to predefined pattern categories; adding patterns requires care with conflict-resolution rules and interactions, so pattern-management tooling would help.
- Natural-language pattern accuracy (93.2% for questions/greetings) reflects limits of rule-based approaches; hybrid regex + statistical/neural methods could help.
- Future work: dynamic pattern learning, GPU/heterogeneous acceleration, semantic conflict resolution, distributed matching across machines for web-scale classification.

## 6. System Execution Evidence

*(Terminal screenshots, transcribed.)* The program is compiled and run as:

```
$ vi complier.c
$ gcc complier.c -o complier
$ ./complier
================================
 PARALLEL TEXT CLASSIFIER SYSTEM
================================
```

Each run prompts `Enter text to classify:`, echoes `INPUT TEXT: "..."`, prints `ANALYZING TEXT PATTERNS IN PARALLEL...`, then one line per core with ✅/❌, then `ANALYSIS COMPLETE`. Core → category mapping: Core 0 EMAIL, Core 1 PHONE, Core 2 QUESTION, Core 3 GREETING, Core 4 NAME. Line order varies between runs (children report as they finish).

| Input | EMAIL (C0) | PHONE (C1) | QUESTION (C2) | GREETING (C3) | NAME (C4) |
|---|---|---|---|---|---|
| `karuna@example.com` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `9471763833` | ❌ | ✅ | ❌ | ❌ | ❌ |
| `Hello what is your name?` | ❌ | ❌ | ✅ | ❌ | ❌ |
| `invalid-email` | ❌ | ❌ | ❌ | ❌ | ❌ |

The screenshots show only per-core match/no-match output — no conflict resolution, confidence score, or timing is printed.

---

## Converter notes (not part of the original paper)

Inconsistencies in the source, worth knowing before reusing its numbers or design:

- Abstract says 96.3% accuracy over 1,250 samples; the accuracy table shows 97.3% over 900.
- Section 4.2 inference quotes 98.7% (structured) / 93.2% (natural language), email precision 99.1%, greeting recall 95.8% — none match the table values.
- "2.4× speedup" is described as "140% improvement in throughput"; 8.9 → 3.7 ms is ~2.4× throughput (a ~140% *increase*), so the wording is loose but roughly consistent.
- Section 4.3 claims a single input takes 7.3 ms on 1 core, but 5.3 says single-text classification sees reduced speedup due to fork overhead — `fork()` per pattern per input is the dominant cost and is the main thing a streaming extension must avoid.
- In the execution evidence, `Hello what is your name?` does **not** match GREETING even though the regex is `^(hello|...)`. That implies the actual program does not lowercase input before matching, contrary to the Input Processing Module. Similarly, the NAME regex `^[A-Z][a-z]+ [A-Z][a-z]+$` could never match after lowercasing — the described normalization and the NAME pattern are mutually incompatible.
- The pseudocode compiles the regex inside each child on every call (`regcomp`/`regfree` per match); dynamic load balancing, heartbeats, auto-restart, and JSON/binary output formats are described but not shown in pseudocode or evidence.
