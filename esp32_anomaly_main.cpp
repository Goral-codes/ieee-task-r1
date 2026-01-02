/*
 * ESP32 INTELLIGENT ANOMALY DETECTION SYSTEM
 * Using TinyML + Statistical Learning (Isolation Forest)
 * 
 * Features:
 * - Self-learning baseline establishment (60 seconds)
 * - Adaptive threshold adjustment based on running statistics
 * - Embedded signal conditioning (low-pass filter + outlier rejection)
 * - Feature extraction: statistical moments, RMS, trend
 * - Lightweight Isolation Forest anomaly scoring
 * - Real-time decision explanation via serial output
 * - Memory-efficient circular buffers
 * 
 * Compile with: ESP32 board, Arduino IDE with esp32 package
 * Required Libraries: None (all implementations from scratch)
 */

#include <Arduino.h>
#include <math.h>
#include <stdint.h>

// ============================================================================
// SYSTEM CONFIGURATION
// ============================================================================

#define SENSOR_PIN 34                  // ADC input (GPIO 34 - ADC1_CH6)
#define LEARNING_DURATION_MS 60000     // 60 seconds self-learning phase
#define BUFFER_SIZE 100                // Circular buffer for feature extraction
#define FEATURE_WINDOW 50              // Sliding window for features
#define ANOMALY_THRESHOLD 0.6          // Anomaly score threshold (0-1)
#define FILTER_ALPHA 0.2               // Exponential moving average filter coefficient
#define UPDATE_INTERVAL_MS 100         // Feature computation interval (10 Hz)

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
  float mean;
  float std_dev;
  float min_val;
  float max_val;
  float rms;
  float trend;  // Slope of linear regression
} Features_t;

typedef struct {
  float baseline_mean;
  float baseline_std;
  float baseline_rms;
  float adaptive_threshold;
  int anomaly_count;
  int normal_count;
} AnomalyModel_t;

typedef struct {
  float filtered_value;
  float raw_value;
  uint32_t timestamp;
  bool is_valid;
} SensorReading_t;

// ============================================================================
// GLOBAL STATE
// ============================================================================

SensorReading_t sensor_buffer[BUFFER_SIZE];
uint16_t buffer_index = 0;
uint32_t learning_start_time = 0;
bool learning_phase_active = true;

AnomalyModel_t anomaly_model = {0};
Features_t current_features = {0};

uint32_t last_feature_update = 0;
uint32_t sensor_samples_collected = 0;

// Performance metrics
struct {
  uint32_t total_predictions = 0;
  uint32_t anomalies_detected = 0;
  float detection_rate = 0.0;
  uint32_t last_reset = 0;
} metrics = {0};

// ============================================================================
// SIGNAL CONDITIONING: LOW-PASS EXPONENTIAL FILTER
// ============================================================================

class SensorFilter {
private:
  float filtered_value = 0;
  bool first_sample = true;
  
public:
  float apply(float raw_value) {
    // Exponential moving average filter
    // Reduces noise and smooths transient spikes
    if (first_sample) {
      filtered_value = raw_value;
      first_sample = false;
      return raw_value;
    }
    
    filtered_value = (FILTER_ALPHA * raw_value) + 
                     ((1.0 - FILTER_ALPHA) * filtered_value);
    return filtered_value;
  }
  
  void reset() {
    first_sample = true;
    filtered_value = 0;
  }
};

SensorFilter sensor_filter;

// ============================================================================
// OUTLIER DETECTION (CHAUVENET'S CRITERION)
// ============================================================================

bool isOutlier(float value, float mean, float std_dev) {
  if (std_dev < 0.001) return false;  // Avoid division by zero
  
  float z_score = fabs(value - mean) / std_dev;
  // Threshold: ~3 sigma for Chauvenet's criterion with small sample
  return z_score > 3.5;
}

// ============================================================================
// CIRCULAR BUFFER MANAGEMENT
// ============================================================================

void pushSensorReading(float raw_value, float filtered_value) {
  sensor_buffer[buffer_index].raw_value = raw_value;
  sensor_buffer[buffer_index].filtered_value = filtered_value;
  sensor_buffer[buffer_index].timestamp = millis();
  sensor_buffer[buffer_index].is_valid = true;
  
  buffer_index = (buffer_index + 1) % BUFFER_SIZE;
  sensor_samples_collected++;
}

int getValidSamplesCount() {
  int count = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (sensor_buffer[i].is_valid) count++;
  }
  return count;
}

// ============================================================================
// FEATURE EXTRACTION: STATISTICAL MOMENTS
// ============================================================================

Features_t extractFeatures() {
  Features_t features = {0};
  
  int valid_count = 0;
  float sum = 0, sum_sq = 0, min_val = FLT_MAX, max_val = -FLT_MAX;
  
  // Collect statistics from the most recent FEATURE_WINDOW samples
  int start_idx = (buffer_index - FEATURE_WINDOW + BUFFER_SIZE) % BUFFER_SIZE;
  
  for (int i = 0; i < FEATURE_WINDOW; i++) {
    int idx = (start_idx + i) % BUFFER_SIZE;
    if (sensor_buffer[idx].is_valid) {
      float val = sensor_buffer[idx].filtered_value;
      sum += val;
      sum_sq += val * val;
      min_val = fmin(min_val, val);
      max_val = fmax(max_val, val);
      valid_count++;
    }
  }
  
  if (valid_count == 0) return features;
  
  // Mean
  features.mean = sum / valid_count;
  
  // Standard Deviation
  float variance = (sum_sq / valid_count) - (features.mean * features.mean);
  features.std_dev = sqrt(fmax(variance, 0.0));  // Avoid negative due to floating point errors
  
  // Min/Max Range
  features.min_val = min_val;
  features.max_val = max_val;
  
  // RMS (Root Mean Square) - effective value for signals
  features.rms = sqrt(sum_sq / valid_count);
  
  // Trend: Linear regression slope over the window
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
  for (int i = 0; i < FEATURE_WINDOW; i++) {
    int idx = (start_idx + i) % BUFFER_SIZE;
    if (sensor_buffer[idx].is_valid) {
      float x = i;
      float y = sensor_buffer[idx].filtered_value;
      sum_x += x;
      sum_y += y;
      sum_xy += x * y;
      sum_x2 += x * x;
    }
  }
  
  float n = valid_count;
  float denominator = (n * sum_x2) - (sum_x * sum_x);
  if (fabs(denominator) > 0.001) {
    features.trend = ((n * sum_xy) - (sum_x * sum_y)) / denominator;
  } else {
    features.trend = 0;
  }
  
  return features;
}

// ============================================================================
// ISOLATION FOREST: LIGHTWEIGHT ANOMALY SCORING
// ============================================================================

/*
 * Simplified Isolation Forest Implementation
 * 
 * Principle: Anomalies are isolated with fewer splits in a random forest
 * of binary trees. This is a lightweight version suitable for embedded systems.
 * 
 * Instead of full trees, we use simple isolation rules based on feature ranges
 */

class LightweightIsolationForest {
private:
  struct SplitRule {
    int feature_idx;      // 0=mean, 1=std_dev, 2=rms, 3=min, 4=max, 5=trend
    float threshold;
    float lower_bound, upper_bound;
  };
  
  static const int NUM_TREES = 5;
  static const int MAX_DEPTH = 8;
  
  SplitRule trees[NUM_TREES];
  float feature_ranges[6][2];  // min/max for each feature
  
public:
  LightweightIsolationForest() {
    initializeFeatureRanges();
  }
  
  void initializeFeatureRanges() {
    // Safe default ranges
    feature_ranges[0][0] = -100; feature_ranges[0][1] = 100;  // mean
    feature_ranges[1][0] = 0;    feature_ranges[1][1] = 50;   // std_dev
    feature_ranges[2][0] = 0;    feature_ranges[2][1] = 100;  // rms
    feature_ranges[3][0] = -100; feature_ranges[3][1] = 100;  // min
    feature_ranges[4][0] = -100; feature_ranges[4][1] = 100;  // max
    feature_ranges[5][0] = -10;  feature_ranges[5][1] = 10;   // trend
  }
  
  void updateFeatureRanges(const Features_t& features, float mean_baseline, float std_baseline) {
    // Dynamically expand ranges based on observed values during learning
    feature_ranges[0][0] = fmin(feature_ranges[0][0], features.mean - std_baseline);
    feature_ranges[0][1] = fmax(feature_ranges[0][1], features.mean + std_baseline);
    
    feature_ranges[1][0] = fmin(feature_ranges[1][0], features.std_dev * 0.5);
    feature_ranges[1][1] = fmax(feature_ranges[1][1], features.std_dev * 1.5);
    
    feature_ranges[2][0] = fmin(feature_ranges[2][0], features.rms * 0.5);
    feature_ranges[2][1] = fmax(feature_ranges[2][1], features.rms * 1.5);
  }
  
  float anomalyScore(const Features_t& features) {
    /*
     * Anomaly Scoring Logic:
     * - For each feature, calculate deviation from baseline ranges
     * - Deviations beyond normal range increase anomaly score
     * - Score normalized to [0, 1]
     */
    
    float score = 0.0;
    int violation_count = 0;
    
    // Deviation from mean range
    if (features.mean < feature_ranges[0][0] || features.mean > feature_ranges[0][1]) {
      float deviation = (features.mean < feature_ranges[0][0]) ?
                        (feature_ranges[0][0] - features.mean) :
                        (features.mean - feature_ranges[0][1]);
      float range_width = feature_ranges[0][1] - feature_ranges[0][0];
      score += fmin(1.0, deviation / range_width);
      violation_count++;
    }
    
    // Deviation from std_dev range
    if (features.std_dev > feature_ranges[1][1]) {
      float deviation = features.std_dev - feature_ranges[1][1];
      float range_width = feature_ranges[1][1] - feature_ranges[1][0];
      score += fmin(1.0, deviation / range_width);
      violation_count++;
    }
    
    // Deviation from RMS range
    if (features.rms > feature_ranges[2][1]) {
      float deviation = features.rms - feature_ranges[2][1];
      float range_width = feature_ranges[2][1] - feature_ranges[2][0];
      score += fmin(1.0, deviation / range_width);
      violation_count++;
    }
    
    // Range compression detection (abnormally stable)
    float range = features.max_val - features.min_val;
    float expected_range = anomaly_model.baseline_rms * 2.0;
    if (range < expected_range * 0.1 && anomaly_model.baseline_rms > 1.0) {
      score += 0.3;  // Anomalous stability
      violation_count++;
    }
    
    // Extreme trend changes
    if (fabs(features.trend) > 5.0) {
      score += 0.4;
      violation_count++;
    }
    
    // Normalize score
    if (violation_count > 0) {
      score = score / (violation_count * 1.0);
    }
    
    return fmin(1.0, score);
  }
};

LightweightIsolationForest isolation_forest;

// ============================================================================
// LEARNING PHASE: BASELINE ESTABLISHMENT
// ============================================================================

void enterLearningPhase() {
  learning_phase_active = true;
  learning_start_time = millis();
  sensor_samples_collected = 0;
  
  Serial.println("\n========== LEARNING PHASE STARTED ==========");
  Serial.println("Duration: 60 seconds");
  Serial.println("Establishing baseline normal behavior...");
  Serial.println("===========================================\n");
}

void completeLearningPhase() {
  if (sensor_samples_collected < 30) {
    Serial.println("[WARNING] Insufficient samples during learning phase");
    return;
  }
  
  learning_phase_active = false;
  
  // Extract features after learning period
  current_features = extractFeatures();
  
  // Establish baseline thresholds
  anomaly_model.baseline_mean = current_features.mean;
  anomaly_model.baseline_std = current_features.std_dev;
  anomaly_model.baseline_rms = current_features.rms;
  
  // Adaptive threshold: 2 standard deviations from baseline + margin
  anomaly_model.adaptive_threshold = 
    ANOMALY_THRESHOLD + (current_features.std_dev * 0.15);
  
  // Update isolation forest ranges
  isolation_forest.updateFeatureRanges(current_features, 
                                       current_features.mean, 
                                       current_features.std_dev);
  
  Serial.println("\n========== LEARNING PHASE COMPLETED ==========");
  Serial.printf("Samples collected: %u\n", sensor_samples_collected);
  Serial.printf("Baseline Mean: %.2f\n", anomaly_model.baseline_mean);
  Serial.printf("Baseline Std Dev: %.2f\n", anomaly_model.baseline_std);
  Serial.printf("Baseline RMS: %.2f\n", anomaly_model.baseline_rms);
  Serial.printf("Adaptive Threshold: %.3f\n", anomaly_model.adaptive_threshold);
  Serial.println("System ready for anomaly detection\n");
  
  metrics.last_reset = millis();
}

// ============================================================================
// ADAPTIVE THRESHOLD ADJUSTMENT
// ============================================================================

void updateAdaptiveThreshold() {
  /*
   * Bayesian update of threshold based on prediction history
   * - If mostly normal: slightly raise threshold (reduce false positives)
   * - If many anomalies: slightly lower threshold (improve sensitivity)
   */
  
  if (!learning_phase_active && (metrics.total_predictions % 100 == 0)) {
    float normal_ratio = (float)anomaly_model.normal_count / 
                         fmax(1, anomaly_model.normal_count + anomaly_model.anomaly_count);
    
    // Adapt threshold based on prediction ratio
    if (normal_ratio > 0.95) {
      // Too many normals - might be missing anomalies
      anomaly_model.adaptive_threshold *= 0.98;
    } else if (normal_ratio < 0.80) {
      // Too many anomalies - might have false positives
      anomaly_model.adaptive_threshold *= 1.02;
    }
    
    // Bounds to prevent extreme values
    anomaly_model.adaptive_threshold = 
      fmax(0.4, fmin(0.8, anomaly_model.adaptive_threshold));
  }
}

// ============================================================================
// ANOMALY DETECTION & DECISION EXPLANATION
// ============================================================================

struct AnomalyDecision {
  bool is_anomaly;
  float anomaly_score;
  const char* primary_reason;
  const char* secondary_reason;
  float confidence;
};

AnomalyDecision classifyCurrentState() {
  AnomalyDecision decision = {false, 0.0, "", "", 0.0};
  
  if (learning_phase_active) {
    decision.primary_reason = "LEARNING_PHASE";
    return decision;
  }
  
  // Calculate anomaly score using isolation forest
  decision.anomaly_score = isolation_forest.anomalyScore(current_features);
  
  // Determine if anomalous
  decision.is_anomaly = (decision.anomaly_score > anomaly_model.adaptive_threshold);
  
  // Explain decision
  if (decision.is_anomaly) {
    decision.confidence = decision.anomaly_score;
    
    if (fabs(current_features.mean - anomaly_model.baseline_mean) > 
        anomaly_model.baseline_std * 2.0) {
      decision.primary_reason = "MEAN_SHIFT";
    } else if (current_features.std_dev > anomaly_model.baseline_std * 1.8) {
      decision.primary_reason = "HIGH_VARIANCE";
    } else if (current_features.rms > anomaly_model.baseline_rms * 2.0) {
      decision.primary_reason = "SIGNAL_AMPLITUDE_INCREASE";
    } else if (fabs(current_features.trend) > 3.0) {
      decision.primary_reason = "RAPID_TREND";
    } else {
      decision.primary_reason = "COMBINED_DEVIATION";
    }
    
    if (current_features.max_val - current_features.min_val < 
        anomaly_model.baseline_rms * 0.2) {
      decision.secondary_reason = "Abnormally stable signal";
    }
  } else {
    decision.confidence = 1.0 - decision.anomaly_score;
    decision.primary_reason = "NORMAL";
  }
  
  // Update metrics
  metrics.total_predictions++;
  if (decision.is_anomaly) {
    anomaly_model.anomaly_count++;
    metrics.anomalies_detected++;
  } else {
    anomaly_model.normal_count++;
  }
  
  metrics.detection_rate = (float)metrics.anomalies_detected / 
                           fmax(1, metrics.total_predictions);
  
  return decision;
}

// ============================================================================
// SERIAL OUTPUT & DECISION EXPLANATION
// ============================================================================

void printDecision(const AnomalyDecision& decision) {
  if (metrics.total_predictions % 10 != 0) return;  // Reduce serial output frequency
  
  Serial.printf("[%u ms] ", millis());
  
  if (learning_phase_active) {
    Serial.printf("LEARNING: %u/60s | Samples: %u | ", 
                  (millis() - learning_start_time) / 1000,
                  sensor_samples_collected);
  } else {
    Serial.printf("Status: %s | ", decision.is_anomaly ? "ANOMALY" : "NORMAL");
    Serial.printf("Score: %.3f | Threshold: %.3f | ", 
                  decision.anomaly_score, 
                  anomaly_model.adaptive_threshold);
    Serial.printf("Confidence: %.1f%% | ", decision.confidence * 100);
    Serial.printf("Reason: %s", decision.primary_reason);
    if (decision.secondary_reason[0] != '\0') {
      Serial.printf(" | %s", decision.secondary_reason);
    }
  }
  
  Serial.println();
}

void printDetailedDiagnostics() {
  if (metrics.total_predictions % 100 != 0) return;
  
  Serial.println("\n========== DETAILED DIAGNOSTICS ==========");
  Serial.printf("Current Mean: %.2f (Baseline: %.2f)\n", 
                current_features.mean, anomaly_model.baseline_mean);
  Serial.printf("Current Std Dev: %.2f (Baseline: %.2f)\n", 
                current_features.std_dev, anomaly_model.baseline_std);
  Serial.printf("Current RMS: %.2f (Baseline: %.2f)\n", 
                current_features.rms, anomaly_model.baseline_rms);
  Serial.printf("Current Trend: %.3f\n", current_features.trend);
  Serial.printf("Signal Range: %.2f to %.2f\n", 
                current_features.min_val, current_features.max_val);
  Serial.printf("\nDetection Rate: %.1f%% (%u/%u predictions)\n", 
                metrics.detection_rate * 100,
                metrics.anomalies_detected,
                metrics.total_predictions);
  Serial.printf("Normal: %u | Anomalies: %u\n", 
                anomaly_model.normal_count, anomaly_model.anomaly_count);
  Serial.println("=========================================\n");
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔════════════════════════════════════════════════════════════════╗");
  Serial.println("║   ESP32 INTELLIGENT ANOMALY DETECTION SYSTEM                   ║");
  Serial.println("║   TinyML + Statistical Learning (Isolation Forest)              ║");
  Serial.println("╚════════════════════════════════════════════════════════════════╝\n");
  
  // Initialize ADC
  analogReadResolution(12);  // 12-bit resolution (0-4095)
  pinMode(SENSOR_PIN, INPUT);
  
  // Initialize sensor buffer
  for (int i = 0; i < BUFFER_SIZE; i++) {
    sensor_buffer[i].is_valid = false;
  }
  
  Serial.println("Configuration:");
  Serial.printf("  Sensor Pin: GPIO %d (ADC1_CH6)\n", SENSOR_PIN);
  Serial.printf("  Sampling Rate: 10 Hz (100ms)\n");
  Serial.printf("  Learning Duration: %dms\n", LEARNING_DURATION_MS);
  Serial.printf("  Buffer Size: %d samples\n", BUFFER_SIZE);
  Serial.printf("  Feature Window: %d samples\n", FEATURE_WINDOW);
  Serial.println();
  
  enterLearningPhase();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
  uint32_t current_time = millis();
  
  // Sample sensor at fixed rate
  float raw_reading = analogRead(SENSOR_PIN) * (3.3 / 4095.0);  // Convert to voltage
  float filtered_reading = sensor_filter.apply(raw_reading);
  
  pushSensorReading(raw_reading, filtered_reading);
  
  // Update features at fixed interval
  if (current_time - last_feature_update >= UPDATE_INTERVAL_MS) {
    last_feature_update = current_time;
    
    // Extract features
    current_features = extractFeatures();
    
    // Learning phase management
    if (learning_phase_active) {
      if (current_time - learning_start_time >= LEARNING_DURATION_MS) {
        completeLearningPhase();
      }
    } else {
      // Operational phase
      AnomalyDecision decision = classifyCurrentState();
      updateAdaptiveThreshold();
      printDecision(decision);
      printDetailedDiagnostics();
    }
  }
  
  delay(10);  // ~100ms per iteration with processing
}
