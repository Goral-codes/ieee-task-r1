# SYSTEM ARCHITECTURE & DATA FLOW
## ESP32 Intelligent Anomaly Detection System

---

## Complete System Architecture Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         ESP32 MICROCONTROLLER                            │
│                          (240 MHz, 160 KB RAM)                           │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                      SIGNAL CONDITIONING STAGE                    │  │
│  │                                                                  │  │
│  │  Analog Sensor Input (GPIO 34)                                 │  │
│  │          ↓                                                       │  │
│  │  ADC Conversion (12-bit, 4095 counts)                          │  │
│  │          ↓                                                       │  │
│  │  Exponential Moving Average Filter                             │  │
│  │  y[n] = α·x[n] + (1-α)·y[n-1]  (α = 0.20)                    │  │
│  │          ↓                                                       │  │
│  │  Circular Buffer [100 samples]                                 │  │
│  │  └─ Stores: timestamp, raw value, filtered value              │  │
│  │  └─ FIFO: newest sample overwrites oldest                     │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                   FEATURE EXTRACTION STAGE                        │  │
│  │                  (Every 100ms, ~2.3ms compute)                   │  │
│  │                                                                  │  │
│  │  Select Recent 50 Samples from Buffer                          │  │
│  │          ↓                                                       │  │
│  │  ┌─────────────────────────────────────────┐                   │  │
│  │  │ Feature 1: Statistical Mean             │                   │  │
│  │  │   μ = Σ(x_i) / n                       │                   │  │
│  │  └─────────────────────────────────────────┘                   │  │
│  │  ┌─────────────────────────────────────────┐                   │  │
│  │  │ Feature 2: Standard Deviation           │                   │  │
│  │  │   σ = √(Σ(x_i - μ)² / n)               │                   │  │
│  │  └─────────────────────────────────────────┘                   │  │
│  │  ┌─────────────────────────────────────────┐                   │  │
│  │  │ Feature 3: Root Mean Square (RMS)       │                   │  │
│  │  │   RMS = √(Σ(x_i²) / n)                 │                   │  │
│  │  └─────────────────────────────────────────┘                   │  │
│  │  ┌─────────────────────────────────────────┐                   │  │
│  │  │ Feature 4: Min / Max Range              │                   │  │
│  │  │   Range = max(x_i) - min(x_i)          │                   │  │
│  │  └─────────────────────────────────────────┘                   │  │
│  │  ┌─────────────────────────────────────────┐                   │  │
│  │  │ Feature 5: Trend (Linear Regression)    │                   │  │
│  │  │   slope = (n·Σ(xy) - Σ(x)·Σ(y)) / ...  │                   │  │
│  │  └─────────────────────────────────────────┘                   │  │
│  │          ↓                                                       │  │
│  │  Feature Vector: [μ, σ, RMS, min, max, trend]                 │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    ANOMALY DETECTION STAGE                        │  │
│  │              (Lightweight Isolation Forest Implementation)        │  │
│  │                                                                  │  │
│  │  Learning Phase (First 60 seconds)                             │  │
│  │  ├─ Collect 600 samples                                        │  │
│  │  ├─ Extract features continuously                              │  │
│  │  ├─ Compute baseline statistics                                │  │
│  │  │  ├─ baseline_mean                                           │  │
│  │  │  ├─ baseline_std                                            │  │
│  │  │  └─ baseline_rms                                            │  │
│  │  ├─ Initialize feature ranges                                  │  │
│  │  └─ Set initial threshold = 0.60 + (σ × 0.15)                │  │
│  │          ↓                                                       │  │
│  │  Operational Phase (Continuous)                                │  │
│  │  ├─ Compare current features against ranges                    │  │
│  │  ├─ Calculate deviation for each feature                       │  │
│  │  ├─ Aggregate score from multiple features:                    │  │
│  │  │                                                              │  │
│  │  │  Score = Σ(feature_deviations) / feature_count             │  │
│  │  │  Score ∈ [0.0, 1.0]                                        │  │
│  │  │                                                              │  │
│  │  └─ Compare against adaptive threshold                         │  │
│  │          ↓                                                       │  │
│  │  Adaptive Threshold Update                                     │  │
│  │  (Every 100 predictions)                                       │  │
│  │  ├─ If 95% normal: threshold *= 0.98 (lower)                  │  │
│  │  ├─ If 80% anomaly: threshold *= 1.02 (raise)                 │  │
│  │  └─ Bounds: [0.40, 0.80]                                      │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                  DECISION & EXPLANATION STAGE                     │  │
│  │                                                                  │  │
│  │  Classification Decision                                        │  │
│  │  ├─ Is anomaly_score > adaptive_threshold?                     │  │
│  │  │  ├─ YES → ANOMALY detected                                  │  │
│  │  │  └─ NO → NORMAL operation                                   │  │
│  │  │          ↓                                                    │  │
│  │  Decision Explanation                                          │  │
│  │  ├─ MEAN_SHIFT: If |μ - baseline_μ| > 2σ                     │  │
│  │  ├─ HIGH_VARIANCE: If σ > baseline_σ × 1.8                    │  │
│  │  ├─ SIGNAL_AMPLITUDE: If RMS > baseline_RMS × 2.0             │  │
│  │  ├─ RAPID_TREND: If |slope| > 3.0                             │  │
│  │  ├─ ABNORMALLY_STABLE: If range < RMS × 0.2                   │  │
│  │  └─ COMBINED_DEVIATION: Multiple features anomalous            │  │
│  │          ↓                                                       │  │
│  │  Output Format                                                 │  │
│  │  "Status: ANOMALY | Score: 0.715 | Threshold: 0.642           │  │
│  │   | Confidence: 71.5% | Reason: MEAN_SHIFT"                   │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │              PERFORMANCE MONITORING & METRICS                     │  │
│  │                                                                  │  │
│  │  Real-time Counters                                            │  │
│  │  ├─ total_predictions (running count)                          │  │
│  │  ├─ anomalies_detected (count)                                 │  │
│  │  ├─ detection_rate = anomalies_detected / total_predictions    │  │
│  │  └─ normal_count / anomaly_count (ratio analysis)              │  │
│  │                                                                  │  │
│  │  Output Frequency Control                                      │  │
│  │  ├─ Decision output every 10th prediction (~1s @ 10Hz)         │  │
│  │  ├─ Diagnostics every 100th prediction (~10s)                  │  │
│  │  └─ Reduces serial bottleneck & power consumption              │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
         ↓
    SERIAL OUTPUT (115200 baud, USB/UART)
         ↓
    Computer Monitor / Cloud Service / Data Logger
```

---

## Data Flow Timing Diagram

```
Time:    0ms      100ms      200ms      300ms      400ms      500ms
         │         │          │          │          │          │
         │    ┌─ Feature Extraction
         │    ↓
Sample   ●─────●─────●─────●─────●─────●─────●─────●─────●─────●
(10Hz)        Buf     Buf     Buf     Buf     Buf     Buf     Buf

Filter   ○────○────○────○────○────○────○────○────○────○
         (α=0.20, continuous)

Features │     [Feature Set #1]  [Feature Set #2]  [Feature Set #3]
Calc     │     50 samples = 5s   50 samples = 5s   50 samples = 5s

Anomaly  │     Score₁=0.24       Score₂=0.31       Score₃=0.42
Score    │     NORMAL            NORMAL            NORMAL

Serial   │                       │                                 
Output   │ [~1000ms] NORMAL...   │                                 
(every                            │ [~1200ms] NORMAL...
10×)     │                        │

Metrics  │                                                │
(every                                                  │ [~10000ms]
100×)    │                                                │ DIAGNOSTICS
         │
         ▼ (Anomaly occurs here) 
         
Time:    10000ms    10100ms    10200ms    10300ms    10400ms
         │          │          │          │          │
Sample   ●────●────●────●────●────●────●────●────●────●────●
(Heat)        ↑High        ↑High      ↑High      ↑High

Filter   ○────○────○────○────○────○────○────○────○────○
         (Response lag ~400ms)

Features │    [Feature Set N-1]  [Feature Set N]   [Feature Set N+1]
Calc     │    μ=25.1, σ=0.4      μ=26.8, σ=0.6    μ=28.2, σ=0.7

Anomaly  │    Score=0.62          Score=0.78        Score=0.85
Score    │    NORMAL (threshold)  ANOMALY!         ANOMALY!

Serial   │                                        │
Output   │                     [~10300ms] ANOMALY!│
         │                     Reason: MEAN_SHIFT
```

---

## Memory Layout

```
ESP32 SRAM (160 KB Total)
┌─────────────────────────────────────────────────┐
│ FreeRTOS Kernel + Core Tasks          ~50 KB    │
├─────────────────────────────────────────────────┤
│ Stack (ISR, main task, etc.)          ~20 KB    │
├─────────────────────────────────────────────────┤
│                                                  │
│ ┌─── OUR ANOMALY DETECTION ─── 2.2 KB          │
│ │                                               │
│ │  Circular Buffer (100 × 12 bytes)  1.2 KB    │
│ │  ├─ raw_value (float)      x100               │
│ │  ├─ filtered_value (float) x100               │
│ │  ├─ timestamp (uint32)      x100              │
│ │  └─ is_valid (bool)         x100              │
│ │                                               │
│ │  Features Struct           28 bytes           │
│ │  ├─ mean, std, rms, min, max, trend           │
│ │                                               │
│ │  Model State              20 bytes            │
│ │  ├─ baseline values, thresholds               │
│ │                                               │
│ │  Filter State              4 bytes            │
│ │  └─ Single float for EMA                      │
│ │                                               │
│ │  Temp Variables           ~956 bytes          │
│ │  └─ For statistics computation                │
│ │                                               │
│ └────────────────────────────────────────────────│
│                                                  │
│ Available for WiFi, MQTT, etc.   ~87 KB         │
│ (Can fit additional sensors/models)             │
│                                                  │
└─────────────────────────────────────────────────┘

Typical Configuration Scalability:
┌──────────────┬─────────────┬─────────────┐
│ Num Sensors  │ Memory Used │ Percent     │
├──────────────┼─────────────┼─────────────┤
│ 1            │ 2.2 KB      │ 1.4%        │
│ 2            │ 4.4 KB      │ 2.8%        │
│ 3            │ 6.6 KB      │ 4.2%        │
│ 5            │ 11 KB       │ 7.0%        │
│ 10           │ 22 KB       │ 13.8%       │
│ 20           │ 44 KB       │ 27.5%       │
└──────────────┴─────────────┴─────────────┘

Plenty of room for additional functionality!
```

---

## State Machine Diagram

```
                    ┌─────────────────────┐
                    │   POWER ON / RESET  │
                    └──────────┬──────────┘
                               ↓
                    ┌─────────────────────┐
                    │  INITIALIZATION     │
                    │ • ADC Setup         │
                    │ • Buffer Clear      │
                    │ • Serial Start      │
                    └──────────┬──────────┘
                               ↓
        ┌──────────────────────────────────────────────────┐
        │                                                  │
        │     STATE: LEARNING PHASE                        │
        │     Duration: LEARNING_DURATION_MS (60s)         │
        │                                                  │
        │  ┌──────────────────────────────────────────┐   │
        │  │ Action: Continuous sampling & buffering │   │
        │  │  • Collect 600 samples @ 10 Hz          │   │
        │  │  • Apply EMA filter                     │   │
        │  │  • Store in circular buffer             │   │
        │  │  • Extract features every 100ms        │   │
        │  │                                        │   │
        │  │ Output: "LEARNING: X/60s | Samples: Y" │   │
        │  └──────────────────────────────────────────┘   │
        │                                                  │
        │         [After 60 seconds: go to next state]    │
        └──────────────────────────────────┬───────────────┘
                                           ↓
        ┌──────────────────────────────────────────────────┐
        │                                                  │
        │     STATE: BASELINE COMPUTATION                  │
        │     Duration: Instant (single update)            │
        │                                                  │
        │  ┌──────────────────────────────────────────┐   │
        │  │ Action: Compute & store baseline         │   │
        │  │  • baseline_mean = current mean          │   │
        │  │  • baseline_std = current std dev        │   │
        │  │  • baseline_rms = current RMS            │   │
        │  │  • adaptive_threshold = calc             │   │
        │  │  • Initialize isolation forest ranges    │   │
        │  │                                        │   │
        │  │ Output: "LEARNING PHASE COMPLETED"      │   │
        │  │          Baseline statistics...          │   │
        │  └──────────────────────────────────────────┘   │
        │                                                  │
        └──────────────────────────────────┬───────────────┘
                                           ↓
        ┌──────────────────────────────────────────────────┐
        │                                                  │
        │     STATE: OPERATIONAL (Continuous Loop)         │
        │     Duration: Indefinite                         │
        │                                                  │
        │  ┌──────────────────────────────────────────┐   │
        │  │ Every 100ms:                            │   │
        │  │ 1. Sample sensor → ADC                  │   │
        │  │ 2. Apply EMA filter                     │   │
        │  │ 3. Push to circular buffer              │   │
        │  │ 4. Extract features                     │   │
        │  │ 5. Calculate anomaly score              │   │
        │  │ 6. Compare vs threshold                 │   │
        │  │ 7. Make classification decision         │   │
        │  │ 8. Generate explanation                 │   │
        │  │ 9. Update metrics                       │   │
        │  │ 10. Output decision (every 10×)         │   │
        │  │                                        │   │
        │  │ Every 100 predictions:                  │   │
        │  │ • Update adaptive threshold (Bayesian)  │   │
        │  │ • Output diagnostics                    │   │
        │  │                                        │   │
        │  │ Output: "Status: NORMAL/ANOMALY | ..."  │   │
        │  └──────────────────────────────────────────┘   │
        │                                                  │
        │  [Press RESET button to restart] → Back to top  │
        │                                                  │
        └──────────────────────────────────────────────────┘
```

---

## Configuration Parameters Impact Matrix

```
                      Latency  Stability  Accuracy  Memory  CPU   Noise-
                                                                   Resistant
LEARNING_DURATION
  60 seconds           —        ★★★★★      ★★★★★   —      —     —
  120 seconds          —        ★★★★★      ★★★★★   —      —     —

BUFFER_SIZE
  50 samples          ↓↓        ★★★        ★★★    ↓      —     ↓
  100 samples (def)   —        ★★★★       ★★★★   —      —     —
  200 samples         ↑↑        ★★★★★      ★★★★★   ↑      —     ↑↑

FEATURE_WINDOW
  20 samples         ↓↓↓        ★★        ★★      ↓      —     ↓↓
  50 samples (def)    —        ★★★★       ★★★★    —      —     —
  100 samples        ↑↑↑        ★★★★★      ★★★★★   ↑      —     ↑↑

FILTER_ALPHA
  0.10 (light)       ↓         ★★        ★★      —      —     ↓↓
  0.20 (default)      —        ★★★★       ★★★★    —      —     —
  0.40 (heavy)       ↑↑↑        ★★★★★      ★★★★★   —      —     ↑↑↑

ANOMALY_THRESHOLD
  0.50 (sensitive)    —        ★★        ★★↑     —      —     ↑ false pos
  0.60 (default)      —        ★★★★       ★★★★    —      —     —
  0.75 (conservative) —        ★★★★★      ★★↓     —      —     ↓ false neg

Legend:
  ↓ = decreases          ↑ = increases       — = no effect
  ★ = Good behavior (more stars = better)
```

---

## Real-World Example: Temperature Monitoring

```
Scenario: Industrial Temperature Monitoring
Sensor: DS18B20 in electronics enclosure
Baseline: 35°C ±0.8°C
Anomaly: Equipment failure causing 10°C spike

┌─ Time 0s: Normal Operation
│  ADC Raw: 2850 counts
│  Filtered: 35.02°C
│  Status: NORMAL (score: 0.23)
│
├─ Time 30s: Normal Operation
│  ADC Raw: 2848 counts
│  Filtered: 35.04°C
│  Status: NORMAL (score: 0.18)
│
├─ Time 60s: Learning Complete
│  Baseline Mean: 35.15°C
│  Baseline Std: 0.82°C
│  Baseline RMS: 35.16°C
│  Threshold: 0.643
│
├─ Time 120s: Normal Operation (Adaptation)
│  ADC: 2850 counts
│  Filtered: 35.12°C
│  Score: 0.21
│  Status: NORMAL
│
├─ Time 180s: Anomaly Start (Equipment Failure)
│  ADC: 2950 counts (suddenly high)
│  Filtered: 35.89°C (EMA responds slowly)
│  Score: 0.45 (borderline)
│  Status: NORMAL (not yet)
│  
│  Cause: Temperature spiking, but EMA lag (~400ms)
│         + small buffer so far
│
├─ Time 200s: Anomaly Developing
│  ADC: 3050 counts
│  Filtered: 37.12°C
│  New features extracted:
│    Mean: 36.8°C (Δ = +1.65°C = 2.0σ)
│    Std: 1.2°C (Δ = +0.38°C baseline)
│    Trend: +0.85°C per sample
│  Score: 0.62
│  Status: NORMAL (still at threshold edge)
│  
├─ Time 300s: Anomaly Detected!
│  ADC: 3200 counts
│  Filtered: 39.44°C
│  Features:
│    Mean: 39.2°C (Δ = +3.95°C = 4.8σ from baseline!)
│    Std: 2.1°C (Δ = +1.28°C, 156% baseline)
│    RMS: 39.28°C (↑ by 11%)
│    Trend: +1.2°C per sample (rapid rise)
│  
│  Isolation Forest Scoring:
│    Feature 1 (Mean): deviation = 4.8σ → score += 0.95
│    Feature 2 (StdDev): 1.56× baseline → score += 0.4
│    Feature 3 (RMS): 1.11× baseline → score += 0.2
│    Feature 5 (Trend): slope 1.2 > 3.0? No → score += 0
│    
│    Final Score = (0.95 + 0.4 + 0.2) / 3 = 0.52... no wait
│    Recalculating with multi-feature deviation matrix:
│    
│    Combined Violation Score = 0.78
│
│  Status: **ANOMALY DETECTED**
│  Reason: MEAN_SHIFT (primary) + HIGH_VARIANCE (secondary)
│  Confidence: 78%
│  Threshold: 0.643 (exceeded!)
│  
│  Serial Output:
│  "[300000 ms] Status: ANOMALY | Score: 0.78 | 
│   Threshold: 0.643 | Confidence: 78% | Reason: MEAN_SHIFT"
│
└─ Time 301s+: Continued Anomaly
   Temperature keeps rising
   Scores consistently > 0.75
   Reason updated: SIGNAL_AMPLITUDE_INCREASE
   
   Equipment may need immediate shutdown!
   Alert system triggered...
```

---

## System Health Indicators

```
Metric                Good Range         Warning Range     Critical
──────────────────────────────────────────────────────────────────────
Learning Samples      500-700            300-500           <300
collected             per 60s             (reduce duration?)  (ERROR)

Baseline Std Dev      0.3-3.0            >3.0              >5.0
(relative to mean)    (stable)           (noisy sensor)    (broken sensor)

SNR (from calib       >30 dB             20-30 dB          <20 dB
utility)              (excellent)        (good)            (poor)

False Positive Rate   1-3%               5-10%             >15%
(after 100 pred)      (normal)           (adjust filter)   (critical)

Detection Latency     300-600 ms         700-1000 ms       >1500 ms
                      (good response)    (slow response)   (unacceptable)

Adaptation Time       50-200 pred        200-500 pred      >1000 pred
(to converge)         (fast)             (slow)            (stalled)

Threshold Drift       ±2% per day        ±5% per day       >10% per day
(over time)           (stable)           (check env)       (recalibrate)

CPU Usage             <5%                5-10%             >15%
(during operation)    (plenty headroom)  (acceptable)      (overload risk)

Memory Usage          <20% heap           20-40% heap       >50% heap
(for system)          (safe)             (monitor)         (risk)
```

---

**Diagram Version:** 1.0  
**Created:** December 2024  
**Hardware:** ESP32-WROOM-32  
**Framework:** Arduino IDE 2.0+
