/*
 * SENSOR CALIBRATION & CONFIGURATION UTILITY
 * ESP32 Anomaly Detection System
 * 
 * This sketch helps you:
 * 1. Calibrate ADC readings to physical units
 * 2. Test sensor signal quality
 * 3. Optimize filter parameters
 * 4. Validate baseline establishment
 * 
 * Run this BEFORE deploying main system
 */

#include <Arduino.h>
#include <math.h>

#define SENSOR_PIN 34
#define NUM_SAMPLES 1000

// Store raw ADC readings
uint16_t adc_samples[NUM_SAMPLES];
float voltage_samples[NUM_SAMPLES];

// ============================================================================
// CALIBRATION STATE
// ============================================================================

struct CalibrationData {
  float adc_min, adc_max;
  float voltage_min, voltage_max;
  float adc_mean, adc_std;
  float voltage_mean, voltage_std;
  float adc_rms;
  float noise_level;
  float snr;  // Signal-to-Noise Ratio
};

CalibrationData calib = {0};

// ============================================================================
// RAW ADC SAMPLING
// ============================================================================

void collectRawSamples() {
  Serial.println("\n========== COLLECTING RAW ADC SAMPLES ==========");
  Serial.println("Sampling 1000 points at ~1kHz...");
  Serial.println("Keep sensor stable during collection\n");
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    adc_samples[i] = analogRead(SENSOR_PIN);
    delayMicroseconds(1000);  // ~1ms per sample
    
    if ((i + 1) % 100 == 0) {
      Serial.printf("Collected %d samples...\n", i + 1);
    }
  }
  
  Serial.println("✓ Sampling complete\n");
}

// ============================================================================
// STATISTICS COMPUTATION
// ============================================================================

void computeStatistics() {
  // Convert to voltages
  float ref_voltage = 3.3;  // Adjust if actual reference differs
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    voltage_samples[i] = adc_samples[i] * (ref_voltage / 4095.0);
  }
  
  // ADC statistics
  float sum_adc = 0, sum_adc_sq = 0;
  calib.adc_min = 4095;
  calib.adc_max = 0;
  
  // Voltage statistics
  float sum_volt = 0, sum_volt_sq = 0;
  calib.voltage_min = 3.3;
  calib.voltage_max = 0;
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    // ADC
    sum_adc += adc_samples[i];
    sum_adc_sq += adc_samples[i] * adc_samples[i];
    calib.adc_min = fmin(calib.adc_min, adc_samples[i]);
    calib.adc_max = fmax(calib.adc_max, adc_samples[i]);
    
    // Voltage
    sum_volt += voltage_samples[i];
    sum_volt_sq += voltage_samples[i] * voltage_samples[i];
    calib.voltage_min = fmin(calib.voltage_min, voltage_samples[i]);
    calib.voltage_max = fmax(calib.voltage_max, voltage_samples[i]);
  }
  
  calib.adc_mean = sum_adc / NUM_SAMPLES;
  calib.voltage_mean = sum_volt / NUM_SAMPLES;
  
  float adc_var = (sum_adc_sq / NUM_SAMPLES) - (calib.adc_mean * calib.adc_mean);
  float volt_var = (sum_volt_sq / NUM_SAMPLES) - (calib.voltage_mean * calib.voltage_mean);
  
  calib.adc_std = sqrt(amax(adc_var, 0.0));
  calib.voltage_std = sqrt(fmax(volt_var, 0.0));
  
  calib.adc_rms = sqrt(sum_adc_sq / NUM_SAMPLES);
  
  // Noise level (high-frequency component)
  float noise_sum = 0;
  for (int i = 1; i < NUM_SAMPLES; i++) {
    float delta = voltage_samples[i] - voltage_samples[i-1];
    noise_sum += delta * delta;
  }
  calib.noise_level = sqrt(noise_sum / (NUM_SAMPLES - 1));
  
  // Signal-to-Noise Ratio
  if (calib.noise_level > 0.001) {
    calib.snr = 20 * log10(calib.voltage_std / calib.noise_level);
  } else {
    calib.snr = 80;  // Very clean signal
  }
}

void printStatistics() {
  Serial.println("========== ADC CALIBRATION STATISTICS ==========\n");
  
  Serial.println("RAW ADC COUNTS (0-4095):");
  Serial.printf("  Min: %u counts\n", (uint16_t)calib.adc_min);
  Serial.printf("  Max: %u counts\n", (uint16_t)calib.adc_max);
  Serial.printf("  Mean: %.1f counts\n", calib.adc_mean);
  Serial.printf("  Std Dev: %.2f counts\n", calib.adc_std);
  Serial.printf("  RMS: %.1f counts\n", calib.adc_rms);
  Serial.printf("  Range: %u counts (%.2f mV)\n", 
                (uint16_t)(calib.adc_max - calib.adc_min),
                (calib.voltage_max - calib.voltage_min) * 1000);
  
  Serial.println("\nVOLTAGE (3.3V reference):");
  Serial.printf("  Min: %.3f V (%.1f mV)\n", calib.voltage_min, calib.voltage_min * 1000);
  Serial.printf("  Max: %.3f V (%.1f mV)\n", calib.voltage_max, calib.voltage_max * 1000);
  Serial.printf("  Mean: %.3f V (%.1f mV)\n", calib.voltage_mean, calib.voltage_mean * 1000);
  Serial.printf("  Std Dev: %.4f V (%.2f mV)\n", calib.voltage_std, calib.voltage_std * 1000);
  
  Serial.println("\nNOISE ANALYSIS:");
  Serial.printf("  Noise Level: %.4f V (%.2f mV)\n", calib.noise_level, calib.noise_level * 1000);
  Serial.printf("  SNR: %.1f dB\n", calib.snr);
  
  // Signal quality assessment
  Serial.println("\nSIGNAL QUALITY ASSESSMENT:");
  if (calib.snr > 40) {
    Serial.println("  ✓ EXCELLENT - Very clean signal");
  } else if (calib.snr > 25) {
    Serial.println("  ✓ GOOD - Adequate for anomaly detection");
  } else if (calib.snr > 15) {
    Serial.println("  ⚠ FAIR - Some noise, increase filter alpha");
  } else {
    Serial.println("  ✗ POOR - High noise, check connections and sensor");
  }
  
  // Recommended filter settings
  Serial.println("\nFILTER RECOMMENDATIONS:");
  if (calib.noise_level < 0.010) {
    Serial.println("  Filter Alpha: 0.15 (minimal filtering needed)");
  } else if (calib.noise_level < 0.030) {
    Serial.println("  Filter Alpha: 0.20 (default, good balance)");
  } else if (calib.noise_level < 0.050) {
    Serial.println("  Filter Alpha: 0.30 (moderate noise)");
  } else {
    Serial.println("  Filter Alpha: 0.40-0.50 (heavy filtering)");
  }
  
  Serial.println("\n===============================================\n");
}

// ============================================================================
// FILTER TESTING
// ============================================================================

class TestFilter {
private:
  float filtered = 0;
  bool first = true;
  float alpha;
  
public:
  TestFilter(float a) : alpha(a) {}
  
  float apply(float raw) {
    if (first) {
      filtered = raw;
      first = false;
      return raw;
    }
    filtered = (alpha * raw) + ((1.0 - alpha) * filtered);
    return filtered;
  }
};

void testFilterResponses() {
  Serial.println("\n========== FILTER RESPONSE TEST ==========\n");
  
  // Test different alpha values
  float test_alphas[] = {0.10, 0.20, 0.30, 0.50};
  
  for (int alpha_idx = 0; alpha_idx < 4; alpha_idx++) {
    float alpha = test_alphas[alpha_idx];
    TestFilter filter(alpha);
    
    float max_error = 0, total_error = 0;
    
    // Apply filter and measure error
    for (int i = 0; i < NUM_SAMPLES; i++) {
      float filtered = filter.apply(voltage_samples[i]);
      float error = fabs(filtered - voltage_samples[i]);
      max_error = fmax(max_error, error);
      total_error += error;
    }
    
    float mean_error = total_error / NUM_SAMPLES;
    
    Serial.printf("Alpha = %.2f:\n", alpha);
    Serial.printf("  Mean Smoothing Error: %.4f V (%.2f mV)\n", mean_error, mean_error * 1000);
    Serial.printf("  Max Smoothing Error: %.4f V (%.2f mV)\n", max_error, max_error * 1000);
    Serial.printf("  Responsiveness: %.1f%%\n", (1.0 - alpha) * 100);
    Serial.println();
  }
}

// ============================================================================
// SENSOR-SPECIFIC CALIBRATION GUIDES
// ============================================================================

void printSensorCalibrationGuides() {
  Serial.println("\n========== SENSOR-SPECIFIC CALIBRATION ==========\n");
  
  Serial.println("FOR LM35 TEMPERATURE SENSOR:");
  Serial.println("  Output: 10mV per °C");
  Serial.println("  Conversion: Temp(°C) = voltage(V) / 0.010");
  Serial.printf("  Your reading: %.1f°C\n", (calib.voltage_mean / 0.010));
  Serial.println("  Calibration: Verify with known temperature (ice bath = 0°C, boiling = 100°C)");
  
  Serial.println("\nFOR LDR (Light Sensor):");
  Serial.println("  Typical R_LDR @ bright: 1-10 kΩ, V_out ≈ 2.5-3.2V");
  Serial.println("  Typical R_LDR @ dark: 100k-1MΩ, V_out ≈ 0.1-0.5V");
  Serial.printf("  Your reading: %.3f V\n", calib.voltage_mean);
  
  float r_ldr = 10000.0 * (3.3 - calib.voltage_mean) / calib.voltage_mean;
  Serial.printf("  Estimated R_LDR: %.0f Ω (%.1f kΩ)\n", r_ldr, r_ldr / 1000);
  
  if (r_ldr < 10000) {
    Serial.println("  Status: ✓ Bright light detected");
  } else if (r_ldr < 100000) {
    Serial.println("  Status: ✓ Moderate light detected");
  } else {
    Serial.println("  Status: ✓ Dark environment detected");
  }
  
  Serial.println("\nFOR BMP280 PRESSURE SENSOR (I2C):");
  Serial.println("  Use Adafruit BMP280 library");
  Serial.println("  Wiring: SDA→GPIO21, SCL→GPIO22");
  Serial.println("  Address: 0x76 (SDO to GND)");
  Serial.println("  Range: 300-1100 hPa");
  
  Serial.println("\n================================================\n");
}

// ============================================================================
// ADC LINEARITY TEST
// ============================================================================

void testADCLinearity() {
  Serial.println("\n========== ADC LINEARITY TEST ==========\n");
  
  // Create histogram of readings
  uint16_t histogram[41] = {0};  // 0-4095 divided into 41 bins
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    uint16_t bin = adc_samples[i] / 100;
    if (bin < 41) histogram[bin]++;
  }
  
  // Find distribution mode
  uint16_t max_count = 0, mode_bin = 0;
  for (int i = 0; i < 41; i++) {
    if (histogram[i] > max_count) {
      max_count = histogram[i];
      mode_bin = i;
    }
  }
  
  Serial.println("Histogram (×=50 samples):");
  for (int i = 0; i < 41; i++) {
    if (histogram[i] > 0) {
      Serial.printf("  %4d-%4d: ", i*100, (i+1)*100-1);
      for (int j = 0; j < histogram[i] / 50; j++) Serial.print("×");
      Serial.printf(" (%d)\n", histogram[i]);
    }
  }
  
  float distribution_width = calib.adc_max - calib.adc_min;
  float distribution_ratio = distribution_width / calib.adc_std;
  
  Serial.printf("\nDistribution Width: %.0f counts (%.1f standard deviations)\n", 
                distribution_width, distribution_ratio);
  
  if (distribution_ratio > 10) {
    Serial.println("⚠ WARNING: Very wide distribution - check for multimodal signals");
  } else if (distribution_ratio < 3) {
    Serial.println("✓ Good: Normal Gaussian-like distribution");
  }
  
  Serial.println();
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 SENSOR CALIBRATION UTILITY      ║");
  Serial.println("║   Anomaly Detection System              ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  analogReadResolution(12);
  pinMode(SENSOR_PIN, INPUT);
  
  Serial.println("Configuration:");
  Serial.printf("  Sensor Pin: GPIO %d\n", SENSOR_PIN);
  Serial.printf("  ADC Resolution: 12-bit (0-4095)\n");
  Serial.printf("  Reference Voltage: 3.3V\n");
  Serial.printf("  Samples for analysis: %d\n\n", NUM_SAMPLES);
  
  // Run complete calibration
  delay(2000);
  collectRawSamples();
  computeStatistics();
  printStatistics();
  testADCLinearity();
  testFilterResponses();
  printSensorCalibrationGuides();
  
  Serial.println("RECOMMENDED CODE SETTINGS FOR MAIN SYSTEM:");
  Serial.println("────────────────────────────────────────────");
  Serial.printf("#define FILTER_ALPHA %.2f\n", 
                (calib.snr > 30) ? 0.20 : (calib.snr > 20) ? 0.25 : 0.30);
  Serial.printf("#define ANOMALY_THRESHOLD %.2f\n",
                (calib.snr > 30) ? 0.60 : 0.55);
  Serial.println("────────────────────────────────────────────\n");
  
  Serial.println("Calibration complete!");
  Serial.println("Deploy main anomaly detection system with settings above.\n");
}

void loop() {
  // Single-run utility
  delay(1000);
}
