#include "daisy_seed.h"

using namespace daisy;
using namespace daisy::seed;
#include "daisysp.h"
#include <cstdlib>  // for rand() and srand()

// Create out Daisy Seed Hardware object
DaisySeed hw;
daisysp::Oscillator osc;
daisysp::Oscillator osc2;
daisysp::Oscillator osc3;  // Sub-bass oscillator for third waveform mode
daisysp::Oscillator lfo;
daisysp::Svf filter;
daisysp::DcBlock dcblock;

enum EnvState {
  ENV_IDLE,
  ENV_ATTACK,
  ENV_SUSTAIN,
  ENV_RELEASE
};

EnvState env_state = ENV_IDLE;
float env = 0.0f;

// Knob setups
AnalogControl tempo_knob;
Parameter tempo_param;
float cutoff = 1000.0f;
float resonance = 0.1f;
float osc_mod_amount = 0.0f;
float attack_mod_amount = 0.0f;  // Attack time modulation from joystick
float max_cutoff = 12000.0f;  // Increased for more high-end range
float min_cutoff = 100.0f;    // Decreased for deeper bass
float cutoff_target = 1000.0f;
float cutoff_smooth = 1000.0f;

// Pitch bend from soft pot gesture
float pitch_bend_amount = 0.0f;  // in semitones, +/- 24 (2 octaves)

// Envelope paramaters
// Attack/Release are hardcoded, sustain is always at 1
// You can adjust sustain time to fill the rest of the step.
float attack_time = 0.01f;   // Shorter for punchier notes
float release_time = 0.08f;  // Slightly longer for smoother tail
float step_length_samples = 0.0f; 
float sustain_samples = 0.0f;
float sustain_counter = 0.0f;

int bitcrush_counter = 0;
float bitcrush_lp = 0.0f;

AnalogControl cutoff_knob;
Parameter cutoff_param;
Parameter resonance_param;

AnalogControl osc_mod_knob;
Parameter osc_mod_param;

AnalogControl attack_mod_knob;
Parameter attack_mod_param;

AnalogControl sustain_knob;
Parameter sustain_param;

AnalogControl cutoff_mod_slider;
Parameter cutoff_mod_slider_param;

// Button setups
Switch delay_button;
GPIO delay_led;
bool delay_enabled = false;

Switch double_tempo_button;
GPIO double_tempo_led;
bool double_tempo_enabled = false;

Switch bitcrush_button;
GPIO bitcrush_led;
bool bitcrush_enabled = false;

Switch waveform_button;
GPIO waveform_led;
int waveform = daisysp::Oscillator::WAVE_SAW; // Init as SAW
int waveform_led_timer = 0;
bool waveform_mixed_mode = false;  // Third mode: saw + square mix

Switch sequence_button;
GPIO sequence_led;
int sequence_led_timer = 0;

Switch swing_button;
bool is_swing = false;
float swing_amount = 0.5;

// Master volume toggle (50% when enabled)
bool half_volume_enabled = false;
int volume_hold_ms = 0;
bool volume_hold_triggered = false;

constexpr int LED_PULSE_MS = 150;

// Setup delay
constexpr size_t MAX_DELAY = 96000;
daisysp::DelayLine<float, MAX_DELAY> delay;

// Delay state
float delay_target = 16000.0f;  // Medium delay time - more audible at slow tempos
float delay_smooth = 16000.0f;
float feedback = 0.2f;
float mix = 0.42f;  // Balanced wet mix for presence without muddiness

//Step timing (the clock)
float phase = 0.0f;

//Tempo smoothing
float bpm_target = 120.0f;
float bpm_smooth = 120.0f;
float steps_per_beat = 2.0f;

constexpr int NUM_STEPS = 8;
float step_freqs[NUM_STEPS];
bool step_is_rest[NUM_STEPS];  // Track which steps are silent
float step_velocity[NUM_STEPS];  // Volume per step (0.5 - 1.0)
int current_step = 0;

// Base frequency of the current step (without pitch bend)
float current_base_freq = 0.0f;

constexpr int MAJOR_SCALE[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int MINOR_SCALE[7] = {0, 2, 3, 5, 7, 8, 10};
constexpr int degree_weights[7] = {3, 1, 2, 1, 3, 1, 1}; // Bias towards, 1, 3, 5

int key_root;
bool is_major;
bool is_bassline = false; // Sequence will alternate between a bassline and melody

// LFO modulation range
float lfo_freq = 0.2f;
float lfo_cutoff_mod = 200.0f;

enum AdcChannel {
  tempo = 0,
  filter_cutoff,
  osc_mod,
  sustain,
  attack_mod,
  cutoff_slider,
  NUM_ADC_CHANNELS
};

// AUDIO FUNCTIONS //

// Update all oscillators' frequencies based on a base frequency
void UpdateOscFrequencies(float base_freq) {
  osc.SetFreq(base_freq);

  float detune_ratio = 1.005f;  // 0.5% detune for subtle beating/chorus
  if (osc_mod_amount > 0.0f) {
    osc2.SetFreq(base_freq * 1.5f * detune_ratio);  // Perfect fifth up + detune
  } else if (osc_mod_amount < 0.0f) {
    osc2.SetFreq(base_freq * (2.0f/3.0f) / detune_ratio);  // Perfect fifth down + detune
  } else {
    osc2.SetFreq(base_freq * detune_ratio);  // Unison + slight detune
  }

  // Sub-bass oscillator one octave down
  osc3.SetFreq(base_freq * 0.5f);
}

void UpdateEnvelope() {
switch (env_state) {
  case ENV_IDLE:
    env = 0.0f;
    break;

  case ENV_ATTACK: {
    // Modulate attack time based on joystick (positive): fast (0.001s) to slow (0.15s)
    float attack_modulated = attack_time;
    if (attack_mod_amount > 0.0f) {
      attack_modulated = attack_time + (attack_mod_amount * 0.14f);
      attack_modulated = fminf(attack_modulated, 0.15f);
    }
    env += 1.0f / (attack_modulated * hw.AudioSampleRate());
    if (env >= 1.0f) {
      env = 1.0f;
      env_state = ENV_SUSTAIN;
      sustain_counter = 0.0f;
    }
    break;
  }

  case ENV_SUSTAIN:
    env = 1.0f;
    sustain_counter += 1.0f;
    if (sustain_counter >= sustain_samples) {
      env_state = ENV_RELEASE;
    }
    break;

  case ENV_RELEASE: {
    // Modulate release time based on joystick (negative): fast (0.08s) to slow (0.5s)
    float release_modulated = release_time;
    if (attack_mod_amount < 0.0f) {
      release_modulated = release_time + (fabs(attack_mod_amount) * 0.42f);
      release_modulated = fminf(release_modulated, 0.5f);
    }
    env -= 1.0f / (release_modulated * hw.AudioSampleRate());
    if(env <= 0.0f) {
      env = 0.0f;
      env_state = ENV_IDLE;
    }
    break;
  }
  }
}

void ResetPhaseCycle() {
  phase = 0;
  current_step = (current_step + 1) % NUM_STEPS;
  
  // Only trigger envelope if step is not a rest
  if (!step_is_rest[current_step]) {
    // Store the un-bent base frequency for this step
    current_base_freq = step_freqs[current_step];

    // Apply pitch bend (convert semitones to frequency ratio)
    float bend_ratio = powf(2.0f, pitch_bend_amount / 12.0f);
    float bent_freq = current_base_freq * bend_ratio;

    // Update oscillator frequencies with pitch bend applied
    UpdateOscFrequencies(bent_freq);

    //retriger the envelope
    env_state = ENV_ATTACK;
  }
}

void UpdateClock() {
  //Update the clock, smoothly
  bpm_smooth += 0.001f * (bpm_target - bpm_smooth);
  
  // Smooth bpm changes to avoid clicks
  step_length_samples = hw.AudioSampleRate() / ((bpm_smooth / 60.0f) * steps_per_beat); 

  // Apply swing to the phase
  bool is_odd_step = current_step % 2 != 0;
  float swing_factor = is_odd_step ? swing_amount : (1.0f - swing_amount);
  float phase_inc = (1.0f / step_length_samples) * swing_factor;
  // Advance phasor once per sample
  phase += phase_inc;

  // Reset the step when the phase goes over 1.0, reset the env step, update the osc detune
  if (phase >= 1.0f) {
    ResetPhaseCycle();
  }
}

float bitcrush_quantize(float in, int bits) {
  float max_level = (1 << bits) - 1;
  float lsb = 1.0f / max_level;
  float dither = ((rand() / (float)RAND_MAX) * 2.0f - 1.0f) * lsb * 0.5f;
  float out = roundf((in + dither) * max_level) / max_level;
  return out;
}

float bitcrush_process(float in, int bits, int &counter, int step)
{
  static float hold = 0.0f;
  if (counter <= 0)
  {
    hold = bitcrush_quantize(in, bits);
    counter = step;
  }
  counter--;

  // Gentle low-pass to reduce aliasing
  const float lp_coeff = 0.2f;
  bitcrush_lp += lp_coeff * (hold - bitcrush_lp);
  return bitcrush_lp;
}

// INIT FUNCTIONS //

void InitSynthElements(int sample_rate) {
  // Init oscillator
  osc.Init(sample_rate);
  osc.SetWaveform(daisysp::Oscillator::WAVE_SAW);
  osc.SetAmp(0.05f);

  osc2.Init(sample_rate);
  osc2.SetWaveform(daisysp::Oscillator::WAVE_SAW);
  osc2.SetAmp(0.08f);

  osc3.Init(sample_rate);
  osc3.SetWaveform(daisysp::Oscillator::WAVE_SQUARE);
  osc3.SetAmp(0.06f);  // Sub-bass level

  // Init lfo
  lfo.Init(sample_rate);
  lfo.SetWaveform(daisysp::Oscillator::WAVE_SIN);
  lfo.SetFreq(lfo_freq);
  lfo.SetAmp(1.0f);

  // Init filter
  filter.Init(sample_rate);

  // Remove DC offset from the output chain
  dcblock.Init(sample_rate);

  // Init delay
  delay.Init();
  delay.SetDelay(delay_smooth);

}

void UpdateWaveform() {
  waveform_button.Debounce();
  if(waveform_button.RisingEdge()) {
    waveform_led_timer = LED_PULSE_MS;
    if (waveform == daisysp::Oscillator::WAVE_SAW && !waveform_mixed_mode) {
      // Mode 2: Pure square wave
      waveform = daisysp::Oscillator::WAVE_SQUARE;
      waveform_mixed_mode = false;
      osc.SetAmp(0.035f);
      osc2.SetAmp(0.056f);
      osc.SetWaveform(daisysp::Oscillator::WAVE_SQUARE);
      osc2.SetWaveform(daisysp::Oscillator::WAVE_SQUARE);
    } else if (waveform == daisysp::Oscillator::WAVE_SQUARE && !waveform_mixed_mode) {
      // Mode 3: Saw with sub-bass for fat low end
      waveform = daisysp::Oscillator::WAVE_SAW;
      waveform_mixed_mode = true;
      osc.SetAmp(0.04f);
      osc2.SetAmp(0.06f);
      osc.SetWaveform(daisysp::Oscillator::WAVE_SAW);
      osc2.SetWaveform(daisysp::Oscillator::WAVE_SAW);
    } else {
      // Mode 1: Pure saw wave
      waveform = daisysp::Oscillator::WAVE_SAW;
      waveform_mixed_mode = false;
      osc.SetAmp(0.05f);
      osc2.SetAmp(0.08f);
      osc.SetWaveform(daisysp::Oscillator::WAVE_SAW);
      osc2.SetWaveform(daisysp::Oscillator::WAVE_SAW);
    }
  }

  if(waveform_led_timer > 0) {
    waveform_led.Write(true);
    waveform_led_timer--;
  } else {
    waveform_led.Write(false);
  }
}

void SetupButtons() {
  // Delay Button
  delay_button.Init(D1, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  delay_led.Init(D2, GPIO::Mode::OUTPUT);

  // Double tempo button
  double_tempo_button.Init(D3, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  double_tempo_led.Init(D4, GPIO::Mode::OUTPUT); 

   // Bitcrush button
  bitcrush_button.Init(D5, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  bitcrush_led.Init (D6, GPIO::Mode::OUTPUT);

  // Waveform button
  waveform_button.Init(D7, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  waveform_led.Init(D8, GPIO::Mode::OUTPUT);


  // New sequence button
  sequence_button.Init(D9, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  sequence_led.Init (D10, GPIO::Mode::OUTPUT);

   // New sequence button
  swing_button.Init(D11, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
}

void SetupKnobs() {

  // AnalogControl
  tempo_knob.Init(
    hw.adc.GetPtr(tempo),
    hw.AudioSampleRate(),
    true
  );

  cutoff_knob.Init(
    hw.adc.GetPtr(filter_cutoff),
    hw.AudioSampleRate(),
    true
  );

  osc_mod_knob.Init(
    hw.adc.GetPtr(osc_mod),
    hw.AudioSampleRate(),
    true
  );

  sustain_knob.Init(
    hw.adc.GetPtr(sustain),
    hw.AudioSampleRate(),
    true
  );

  attack_mod_knob.Init(
    hw.adc.GetPtr(attack_mod),
    hw.AudioSampleRate(),
    true
  );

  cutoff_mod_slider.Init(
    hw.adc.GetPtr(cutoff_slider),
    hw.AudioSampleRate(),
    true
  );

  // Map knob to frequency range
  tempo_param.Init(tempo_knob, 80.0f, 320.0f, Parameter::LINEAR);

  // Cutoff and resonance are a single knob macro
  cutoff_param.Init(cutoff_knob, min_cutoff, max_cutoff, Parameter::LOGARITHMIC);
  resonance_param.Init(cutoff_knob, 0.05f, 0.95f, Parameter::LINEAR);  // Wider resonance range

  // Sustain time, not volume
  sustain_param.Init(sustain_knob, 0.05f, 1.0, Parameter::LINEAR);

  // Osc detune (joystick)
  osc_mod_param.Init(osc_mod_knob, -1.0f, 1.0f, Parameter::LINEAR);
  attack_mod_param.Init(attack_mod_knob, -1.0f, 1.0f, Parameter::LINEAR);

  // Cutoff Mod slider (softpot)
  cutoff_mod_slider_param.Init(cutoff_mod_slider, 0.0f, 1.0f, Parameter::LINEAR);

}

// CONTROL FUNCTIONS //

void GenerateSequence() {
  is_major = rand() % 2 == 0;
  is_bassline = !is_bassline;
  int starting_note = is_bassline? 24 : 36;  // Shifted down one octave
  key_root = starting_note + (rand() % 7);

  const int* scale = is_major ? MAJOR_SCALE : MINOR_SCALE;
  int prev_degree = 0;
  
  // Choose melodic contour: 0=climb, 1=fall, 2=arch, 3=random walk
  int contour = rand() % 4;
  
  for (int i = 0; i < NUM_STEPS; i++) {
    int degree = 0;
    
    // First note is always root
    if (i == 0) {
      degree = 0;
    } 
    // Last note resolves
    else if (i == NUM_STEPS - 1) {
      degree = (rand() % 2 == 0) ? 0 : 4;  // Root or dominant
    }
    // Middle notes follow contour
    else {
      switch(contour) {
        case 0: // Climb up
          degree = (i * 7) / NUM_STEPS;
          break;
        case 1: // Fall down
          degree = 6 - ((i * 6) / NUM_STEPS);
          break;
        case 2: // Arch (up then down)
          degree = (i < NUM_STEPS/2) ? (i * 2) : (6 - (i - NUM_STEPS/2) * 2);
          break;
        case 3: // Random walk
          int step_change = (rand() % 3) - 1;
          degree = prev_degree + step_change;
          break;
      }
      
      // Allow repeated notes sometimes
      if (rand() % 4 == 0) {
        degree = prev_degree;
      }
      
      // Clamp to scale
      if (degree < 0) degree = 0;
      if (degree > 6) degree = 6;
    }
    prev_degree = degree;

    // Octave jumps on strong beats (steps 0, 4)
    int octave = 0;
    if (!is_bassline && (i == 0 || i == 4) && rand() % 3 == 0) {
      octave = 12;
    }
    
    int note = key_root + scale[degree] + octave;
    step_freqs[i] = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    
    // Add rests: 15% chance, but never on first or last step
    if (i > 0 && i < NUM_STEPS - 1 && rand() % 100 < 15) {
      step_is_rest[i] = true;
    } else {
      step_is_rest[i] = false;
    }
    
    // Velocity: accent strong beats (0, 4), softer on off-beats
    if (i % 4 == 0) {
      step_velocity[i] = 0.9f + (rand() % 10) / 100.0f;  // 0.9-1.0
    } else if (i % 2 == 0) {
      step_velocity[i] = 0.75f + (rand() % 10) / 100.0f;  // 0.75-0.85
    } else {
      step_velocity[i] = 0.6f + (rand() % 10) / 100.0f;  // 0.6-0.7
    }
  }
}

void UpdateTempo() {
  tempo_knob.Process();
  float target_tempo = tempo_param.Process();
  double_tempo_button.Debounce();
  
  // Toggle double tempo on button press
  if(double_tempo_button.RisingEdge()) {
    double_tempo_enabled = !double_tempo_enabled;
  }

  // If the modify button is pressed, double the tempo. Otherwise, use the target tempo
  bpm_target = double_tempo_enabled ? target_tempo * 2.0f : target_tempo;
  double_tempo_led.Write(double_tempo_enabled);
}

void UpdateSequence() {
  sequence_button.Debounce();
  if(sequence_button.RisingEdge()) {
    sequence_led_timer = LED_PULSE_MS;
    GenerateSequence();
  }

  if(sequence_led_timer > 0) {
    sequence_led.Write(true);
    sequence_led_timer--;
  } else {
    sequence_led.Write(false);
  }
}

void UpdateSustainTime() {
  float sustain_fraction = sustain_param.Process();
  sustain_samples = sustain_fraction * step_length_samples;
}

void UpdateFilterMacro() {
  cutoff_knob.Process();
  cutoff_target = cutoff_param.Process();
  resonance = resonance_param.Process();
}

void UpdatePitchBend() {
  cutoff_mod_slider.Process();
  float softpot = cutoff_mod_slider_param.Process();

  // Touch detected (> 0.01)
  if(softpot > 0.01f) {
    // Map position to pitch bend: 0.01-0.75 range (working area of soft pot)
    // Use full range with logarithmic response away from center
    float min_val = 0.01f;
    float max_val = 0.75f;
    float center = (min_val + max_val) / 2.0f;  // 0.38
    
    // Normalize to -1.0 to +1.0 centered at middle
    float normalized = (softpot - center) / (max_val - center);
    normalized = fminf(fmaxf(normalized, -1.0f), 1.0f);
    
    // Apply exponential curve: sign(x) * (2^(abs(x)*3) - 1) / 7
    // This gives logarithmic response: gentle near center, dramatic at edges
    float sign = (normalized >= 0.0f) ? 1.0f : -1.0f;
    float abs_norm = fabsf(normalized);
    float curved = sign * (powf(2.0f, abs_norm * 3.0f) - 1.0f) / 7.0f;
    
    pitch_bend_amount = curved * 24.0f;  // Scale to +/- 24 semitones
  } else {
    // Finger lifted - no bend
    pitch_bend_amount = 0.0f;
  }

  // Immediately apply pitch bend to the currently playing note
  if (current_base_freq > 0.0f) {
    float bend_ratio = powf(2.0f, pitch_bend_amount / 12.0f);
    float bent_freq = current_base_freq * bend_ratio;
    UpdateOscFrequencies(bent_freq);
  }
}

void UpdateDetuneMod() {
  osc_mod_knob.Process();
  osc_mod_amount = osc_mod_param.Process();
}

void UpdateAttackMod() {
  attack_mod_knob.Process();
  attack_mod_amount = attack_mod_param.Process();
}

void UpdateSwing() {
  swing_button.Debounce();
  if(swing_button.RisingEdge()) {
    is_swing = !is_swing;
    swing_amount = is_swing ? 0.6 : 0.5;
  }
}

void UpdateDelay() {
  delay_button.Debounce();
  
  // Toggle delay on button press
  if(delay_button.RisingEdge()) {
    delay_enabled = !delay_enabled;
  }
  
  delay_led.Write(delay_enabled);
}

void UpdateBitcrush() {
  bitcrush_button.Debounce();
  
  // Toggle bitcrush on button press
  if(bitcrush_button.RisingEdge()) {
    bitcrush_enabled = !bitcrush_enabled;
  }
  
  bitcrush_led.Write(bitcrush_enabled);
}

// SPECIAL FEATURE: hold Delay + Double Tempo for 3 seconds to toggle master volume to 50%
void UpdateVolumeToggle() {
  // Both buttons are momentary with Debounce() already called in UpdateTempo/UpdateDelay
  bool both_pressed = delay_button.Pressed() && double_tempo_button.Pressed();

  if (both_pressed) {
    // Count milliseconds while both buttons are held
    if (volume_hold_ms < 3000) {
      volume_hold_ms++;
    }

    // After ~3 seconds, toggle volume once per hold
    if (volume_hold_ms >= 3000 && !volume_hold_triggered) {
      half_volume_enabled = !half_volume_enabled;
      volume_hold_triggered = true;
    }
  } else {
    // Reset when either button is released
    volume_hold_ms = 0;
    volume_hold_triggered = false;
  }
}

void MyCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
  for (size_t i = 0; i < size; i++) {

    UpdateClock();

    // Calculate the envelope
    UpdateEnvelope();

    // Update the signal, and warm it up & drive
    float sig = osc.Process();
    float sig2 = osc2.Process();
    float sig3 = osc3.Process();
    
    // Apply consistent drive to all waveforms
    float osc_drive = 2.0f;
    sig = tanhf(sig * osc_drive);
    sig2 = tanhf(sig2 * osc_drive);
    sig3 = tanhf(sig3 * osc_drive);

    // Update the lfo
    float lfo_sig = lfo.Process();

    // Add in detune osc with level compensation to prevent clipping
    float detune_amount = fabs(osc_mod_amount);
    sig = (sig * (1.0f - detune_amount * 0.3f)) + (sig2 * detune_amount * 0.7f);
    
    // Add sub-bass ONLY when in saw+sub mode (mode 3)
    if (waveform_mixed_mode && waveform == daisysp::Oscillator::WAVE_SAW) {
      sig = sig * 0.7f + sig3 * 0.5f;
    }

    // Apply the filter
    cutoff_smooth += 0.002f * (cutoff_target - cutoff_smooth);
    // Compute and clamp filter frequency to a safe audible range
    // Gate LFO modulation by envelope to prevent wandering during silence
    float env_gate = fmaxf(env, 0.1f);  // Minimum 10% modulation depth
    float cutoff_modulated = (cutoff_smooth + (500.0f * lfo_sig * env_gate)) + (env * 1000.0f);
    cutoff_modulated = fminf(fmaxf(cutoff_modulated, 20.0f), 12000.0f);
    filter.SetFreq(cutoff_modulated);
    // Keep resonance within a stable range
    float res_mod = resonance + (lfo_sig * 0.02f * env_gate);  // Increased from 0.01 and gated
    res_mod = fminf(fmaxf(res_mod, 0.1f), 0.98f);
    filter.SetRes(res_mod);
    filter.Process(sig);
    float out_sig = filter.Low();

    // Add bitcrush to mix;
    if(bitcrush_enabled) {
      int bits = 8; // Slightly higher resolution for a gentler effect
      int step = static_cast<int>(step_length_samples / 128.0f);
      step = fminf(fmaxf(step, 2), 8); // Shorter hold time for less aggressive crush
      out_sig = bitcrush_process(out_sig, bits, bitcrush_counter, step);
    }

    // Post-filter saturation for warmth and character
    out_sig = tanhf(out_sig * 1.2f) * 0.9f;  // Gentle saturation

    // Filter drive
    float filter_drive = 0.65f;  // Increased output level
    out_sig *= filter_drive;

    // Envelope is applied to the dry signal

    // Amp modulation with per-step velocity
    float velocity = step_is_rest[current_step] ? 0.0f : step_velocity[current_step];
    float mod_amp = env * velocity * (1.0f + lfo_sig * 0.05f); // 5% amplitude swing for subtle movement
    out_sig *= mod_amp;
    
    // Noise gate: cut signal when envelope is very low
    if (env < 0.005f) {
      out_sig *= env / 0.005f;  // Fade to zero below threshold
    }

    // Add delay after envelope so repeats can ring out independently
    if(delay_enabled) {
      delay_smooth += 0.0003f * (delay_target - delay_smooth);
      float delayed = delay.Read(delay_smooth);
      
      // High-pass filter the delayed signal to reduce muddiness
      static float hp_delayed = 0.0f;
      float hp_coeff = 0.92f;  // Gentler high-pass to keep some warmth
      hp_delayed = hp_coeff * (hp_delayed + delayed - delayed);
      delayed = delayed - hp_delayed;
      
      // Write envelope-shaped signal to delay for natural decay
      float delay_feedback = 0.30f;  // More repeats for richer delay
      delay.Write(out_sig + (delayed * delay_feedback));
      out_sig = out_sig * (1-mix) + (delayed * mix);
    }
    
    out_sig = dcblock.Process(out_sig);
    
    // Gentle one-pole low-pass to roll off high-frequency hiss (8kHz-ish)
    static float hp_smooth = 0.0f;
    float lp_coeff = 0.7f;  // Adjusts cutoff frequency
    hp_smooth = hp_smooth * lp_coeff + out_sig * (1.0f - lp_coeff);
    out_sig = hp_smooth;

    // Apply master volume toggle (50% when enabled)
    float master_gain = half_volume_enabled ? 0.5f : 1.0f;
    out_sig *= master_gain;
    
    //out_sig *= 1.5f; // Master volume boost

    out[0][i] = out_sig;
    out[1][i] = out_sig;
  }
}


int main(void) {

  // Initialize the Daisy Seed hardware
  hw.Configure();
  hw.Init();
  // hw.StartLog();  // Disabled - causes USB instability during audio

  // Setup oscillators, filters, and delay
  int sample_rate = hw.AudioSampleRate();
  InitSynthElements(sample_rate);

  // Create an ADC Channel Config object
  AdcChannelConfig adc_config[NUM_ADC_CHANNELS];

  adc_config[filter_cutoff].InitSingle(A0);
  adc_config[sustain].InitSingle(A1);
  adc_config[tempo].InitSingle(A2);
	adc_config[osc_mod].InitSingle(A3);
	adc_config[attack_mod].InitSingle(A4);
  adc_config[cutoff_slider].InitSingle(A5);
  hw.adc.Init(adc_config, NUM_ADC_CHANNELS);
  hw.adc.Start();

  // Configure the UI controls
  SetupButtons();
  SetupKnobs();

  // Random seed so we get different patterns
  tempo_knob.Process();
  srand(tempo_param.Process());

  // Setup the inital sequence
  GenerateSequence();

  // Start the audio
  hw.StartAudio(MyCallback);

  // bool piezo_armed = true;
  // uint32_t piezo_cooldown = 0;

  while (1) {
    // Check all controls and update the state of the synth accordingly
    UpdateTempo();
    UpdateWaveform();
    UpdateSequence();
    UpdateSustainTime();
    UpdateFilterMacro();
    UpdatePitchBend();
    UpdateDetuneMod();
    UpdateAttackMod();
    UpdateDelay();
    UpdateBitcrush();
    UpdateSwing();
    UpdateVolumeToggle();

    System::Delay(1);
  }
}
