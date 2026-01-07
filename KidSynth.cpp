#include "daisy_seed.h"

using namespace daisy;
using namespace daisy::seed;
#include "daisysp.h"

// Create out Daisy Seed Hardware object
DaisySeed hw;
daisysp::Oscillator osc;
daisysp::Oscillator osc2;
daisysp::Svf filter;

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
float cutoff_mod_amount = 0.0f;
float max_cutoff = 8000.0f;
float min_cutoff = 200.0f;

// Envelope paramaters
// Attack/Release are hardcoded, sustain is always at 1
// You can adjust sustain time to fill the rest of the step.
float attack_time = 0.05f;
float release_time = 0.05f;
float step_length_samples = 0.0f; 
float sustain_samples = 0.0f;
float sustain_counter = 0.0f;

int bitcrush_counter = 0;

AnalogControl cutoff_knob;
Parameter cutoff_param;
Parameter resonance_param;

AnalogControl osc_mod_knob;
Parameter osc_mod_param;

AnalogControl cutoff_mod_knob;
Parameter cutoff_mod_param;

AnalogControl sustain_knob;
Parameter sustain_param;

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

Switch sequence_button;
GPIO sequence_led;

Switch swing_button;
bool is_swing = false;
float swing_amount = 0.5;

// Setup delay
constexpr size_t MAX_DELAY = 96000;
daisysp::DelayLine<float, MAX_DELAY> delay;

// Delay state
float delay_target = 15000.0f; 
float delay_smooth = 15000.0f;
float feedback = 0.2f;
float mix = 0.3f;

//Step timing (the clock)
float phase = 0.0f;

//Tempo smoothing
float bpm_target = 120.0f;
float bpm_smooth = 120.0f;
float steps_per_beat = 2.0f;

constexpr int NUM_STEPS = 8;
float step_freqs[NUM_STEPS];
int current_step = 0;

constexpr int MAJOR_SCALE[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int MINOR_SCALE[7] = {0, 2, 3, 5, 7, 8, 10};
constexpr int degree_weights[7] = {3, 1, 2, 1, 3, 1, 1}; // Bias towards, 1, 3, 5

int key_root;
bool is_major;
bool is_bassline = false; // Sequence will alternate between a bassline and melody

enum AdcChannel {
  tempo = 0,
  filter_cutoff,
  osc_mod,
  sustain,
  cutoff_mod,
  NUM_ADC_CHANNELS
};

// AUDIO FUNCTIONS //

void UpdateEnvelope() {
switch (env_state) {
  case ENV_IDLE:
    env = 0.0f;
    break;

  case ENV_ATTACK: 
    env += 1.0f / (attack_time * hw.AudioSampleRate());
    if (env >= 1.0f) {
      env = 1.0f;
      env_state = ENV_SUSTAIN;
      sustain_counter = 0.0f;
    }
    break;

  case ENV_SUSTAIN:
    env = 1.0f;
    sustain_counter += 1.0f;
    if (sustain_counter >= sustain_samples) {
      env_state = ENV_RELEASE;
    }
    break;

  case ENV_RELEASE:
    env -= 1.0f / (release_time * hw.AudioSampleRate());
    if(env <= 0.0f) {
      env = 0.0f;
      env_state = ENV_IDLE;
    }
    break;
  }
}

void ResetPhaseCycle() {
  phase = 0;
  current_step = (current_step + 1) % NUM_STEPS;
  osc.SetFreq(step_freqs[current_step]); // Update the playing note

  //update the detune osc
  float detune = 0.5f;
  if (osc_mod_amount > 0.0f) {
    osc2.SetFreq(step_freqs[current_step] * 1.5 - detune);
  } else if (osc_mod_amount < 0.0f) {
    osc2.SetFreq(step_freqs[current_step] * (2.0f/3.0f) - detune);
  } else {
    osc2.SetFreq(step_freqs[current_step]);
  }

  //retriger the envelope
  env_state = ENV_ATTACK;

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
  phase += phase_inc;

  //Advance phasor
  phase += phase_inc;

  // Reset the step when the phase goes over 1.0, reset the env step, update the osc detune
  if (phase >= 1.0f) {
    ResetPhaseCycle();
  }
}

float bitcrush(float in, int bits) {
  float max_level = (1 << bits) - 1;
  float out = roundf(in * max_level) / max_level;
  return out;
}

float bitcrush_sample(float in, int &counter, int step)
{
  static float hold = 0.0f;
  if (counter == 0)
  {
    hold = in;
    counter = step;
  }
  counter--;
  return hold;
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

  // Init filter
  filter.Init(sample_rate);

  // Init delay
  delay.Init();
  delay.SetDelay(delay_smooth);

}

void UpdateWaveform() {
  waveform_button.Debounce();
  waveform_led.Write(waveform_button.Pressed());

    // Hardcode cycle between waveforms.
    if(waveform_button.RisingEdge()) {
      if (waveform == daisysp::Oscillator::WAVE_SAW) {
        waveform = daisysp::Oscillator::WAVE_SQUARE;
      } else if (waveform == daisysp::Oscillator::WAVE_SQUARE) {
        waveform = daisysp::Oscillator::WAVE_TRI;
      } else {
        waveform = daisysp::Oscillator::WAVE_SAW;
      }

      osc.SetWaveform(waveform);
      // Keep the detune osc the same as the main osc
      osc2.SetWaveform(waveform);
    }
}

void SetupButtons() {
  // Delay Button
  delay_button.Init(D1, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  delay_led.Init(D2, GPIO::Mode::OUTPUT);

  // Double tempo button
  double_tempo_button.Init(D3, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  double_tempo_led.Init(D4, GPIO::Mode::OUTPUT); 

  // Waveform button
  waveform_button.Init(D5, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  waveform_led.Init (D6, GPIO::Mode::OUTPUT);

   // Bitcrush button
  bitcrush_button.Init(D7, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  bitcrush_led.Init (D8, GPIO::Mode::OUTPUT);

  // New sequence button
  sequence_button.Init(D9, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
  sequence_led.Init (D10, GPIO::Mode::OUTPUT);

   // New sequence button
  swing_button.Init(D12, hw.AudioSampleRate(), Switch::TYPE_MOMENTARY, Switch::POLARITY_INVERTED, Switch::PULL_UP);
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

  cutoff_mod_knob.Init(
    hw.adc.GetPtr(cutoff_mod),
    hw.AudioSampleRate(),
    true
  );

  // Map knob to frequency range
  tempo_param.Init(tempo_knob, 40.0f, 240.0f, Parameter::LOGARITHMIC);

  // Cutoff and resonance are a single knob macro
  cutoff_param.Init(cutoff_knob, min_cutoff, max_cutoff, Parameter::LOGARITHMIC);
  resonance_param.Init(cutoff_knob, 0.1f, 0.9f, Parameter::LINEAR);

  // Sustain time, not volume
  sustain_param.Init(sustain_knob, 0.05f, 1.0, Parameter::LINEAR);

  // Osc detune (joystick)
  osc_mod_param.Init(osc_mod_knob, -1.0f, 1.0f, Parameter::LINEAR);
  float cutoff_range = (max_cutoff - min_cutoff)/2;
  cutoff_mod_param.Init(cutoff_mod_knob, -1.0*cutoff_range, cutoff_range, Parameter::LINEAR);

}

// CONTROL FUNCTIONS //

void GenerateSequence() {
  is_major = rand() % 2 == 0;
  is_bassline = !is_bassline;
  int starting_note = is_bassline? 36 : 48;
  key_root = starting_note + (rand() % 7);

  const int* scale = is_major ? MAJOR_SCALE : MINOR_SCALE;
  int prev_degree = 0;
  for (int i = 0; i < NUM_STEPS; i++) {

    int degree = rand() % 7;
    // first note is always degree
    if (i == 0) {
      degree = 0;
    } else if (i == NUM_STEPS - 1) {
      // end on dominant
      degree = 4;
    } else {
      int step_change = (rand() %3) - 1;
      degree = prev_degree + step_change;
      if (degree < 0) degree = 0;
      if (degree > 6) degree = 6;
    }
    prev_degree = degree;

    int octave = 0;
    if (rand() % 4 == 0) octave = 12;
    int note = key_root + scale[degree] + octave;

    // Convert MIDI note to frequency
    step_freqs[i] = 440.0f * powf(2.0f, (note - 69) / 12.0f);
  }
}

void UpdateTempo() {
  tempo_knob.Process();
  float target_tempo = tempo_param.Process();
  double_tempo_button.Debounce();
  double_tempo_enabled = double_tempo_button.Pressed();

  // If the modify button is pressed, double the tempo. Otherwise, use the target tempo
  bpm_target = double_tempo_enabled ? target_tempo * 2.0f : target_tempo;
  double_tempo_led.Write(double_tempo_enabled);
}

void UpdateSequence() {
  sequence_button.Debounce();
  sequence_led.Write(sequence_button.Pressed());
  if(sequence_button.RisingEdge()) {
    GenerateSequence();
  }
}

void UpdateSustainTime() {
  float sustain_fraction = sustain_param.Process();
  sustain_samples = sustain_fraction * step_length_samples;
}

void UpdateFilterMacro() {
  cutoff_knob.Process();
  cutoff = cutoff_param.Process();
  resonance = resonance_param.Process();
}

void UpdateFilterMod() {
  cutoff_mod_knob.Process();
  float cutoff_mod_value = cutoff_mod_param.Process();
  cutoff += cutoff_mod_value;
  // Clamp to reasonable values, but beyond the standard range
  cutoff = fminf(fmaxf(cutoff, 200), 12000);
}

void UpdateDetuneMod() {
  osc_mod_knob.Process();
  osc_mod_amount = osc_mod_param.Process();
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
  delay_enabled = delay_button.Pressed();
  delay_led.Write(delay_enabled);
}

void UpdateBitcrush() {
  bitcrush_button.Debounce();
  bitcrush_enabled = bitcrush_button.Pressed();
  bitcrush_led.Write(bitcrush_enabled);
}


void MyCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
  for (size_t i = 0; i < size; i++) {

    UpdateClock();

    // Calculate the envelope
    UpdateEnvelope();

    // Update the signal, and warm it up & drive
    float sig = osc.Process();
    float osc_drive = 3.0f;
    sig = tanhf(sig * osc_drive);

    // Add in detune osc
    sig = sig + (osc2.Process() * fabs(osc_mod_amount));

    // Apply the envlope 
    sig *= env;

    // Apply the filter
    filter.SetFreq(cutoff);
    filter.SetRes(resonance);
    filter.Process(sig);
    float out_sig = filter.Low();

    // Add bitcrush to mix;
    if(bitcrush_enabled) {
      out_sig = bitcrush(out_sig, 4);
      out_sig = bitcrush_sample(out_sig, bitcrush_counter, 4);
    }

    // Filter drive
    float filter_drive = 0.5f;
    out_sig *= filter_drive;

    // Add delay to mix
    if(delay_enabled) {
      delay_smooth += 0.0003f * (delay_target - delay_smooth);
      float delayed = delay.Read(delay_smooth);
      delay.Write(out_sig + (delayed * feedback));
      out_sig = out_sig * (1-mix) + (delayed * mix);
    }

    out[0][i] = out_sig;
    out[1][i] = out_sig;
  }
}


int main(void) {

  // Initialize the Daisy Seed hardware
  hw.Configure();
  hw.Init();
  hw.StartLog();

  // Setup oscillators, filters, and delay
  int sample_rate = hw.AudioSampleRate();
  InitSynthElements(sample_rate);

  // Create an ADC Channel Config object
  AdcChannelConfig adc_config[NUM_ADC_CHANNELS];
  adc_config[tempo].InitSingle(A0);
  adc_config[filter_cutoff].InitSingle(A4);
	adc_config[osc_mod].InitSingle(A1);
	adc_config[cutoff_mod].InitSingle(A2);
  adc_config[sustain].InitSingle(A3);
  hw.adc.Init(adc_config, NUM_ADC_CHANNELS);
  hw.adc.Start();

  // Configure the UI controls
  SetupButtons();
  SetupKnobs();

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
    UpdateFilterMod();
    UpdateDetuneMod();
    UpdateDelay();
    UpdateBitcrush();
    UpdateSwing();
    
    // Check piezo input
    // float piezo_threshold = 0.05f;

    // snare_in.Process();
    // float snare_val = snare_in.Value();
    // hw.PrintLine("anv %f", snare_val);


    // if(piezo_cooldown > 0) 
    // {
    //   piezo_cooldown--;
    // }

    // if(snare_val > piezo_threshold && piezo_armed && piezo_cooldown == 0) {
    //   piezo_armed = false;
    //   piezo_cooldown = 2000;

    //   TriggerSample();
    // }

    // if(snare_val < 0.02f) {
    //   piezo_armed = true;
    // }

    // In order to know that everything's working let's print that to a serial console:
    // hw.PrintLine("anv %f", env_time);
    // hw.PrintLine("attack %f", swing_amount);

    System::Delay(1);
  }
}
