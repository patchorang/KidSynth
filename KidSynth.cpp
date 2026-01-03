#include "daisy_seed.h"

using namespace daisy;
using namespace daisy::seed;
#include "daisysp.h"

// Create out Daisy Seed Hardware object
DaisySeed hw;
daisysp::Oscillator osc;
daisysp::Oscillator osc2;
daisysp::Svf filter;

// Knob setups
AnalogControl tempo_knob;
Parameter tempo_param;
float cutoff = 1000.0f;
float resonance = 0.1f;
float osc_mod_amount = 0.0f;
float resonance_mod_amount = 0.0f;
float max_cutoff = 8000.0f;
float min_cutoff = 200.0f;

AnalogControl cutoff_knob;
Parameter cutoff_param;
Parameter resonance_param;

AnalogControl osc_mod_knob;
Parameter osc_mod_param;

AnalogControl resonance_mod_knob;
Parameter resonance_mod_param;

// AnalogControl waveform_knob;
// Parameter waveform_param;

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

enum WAVEFORMS {
  saw = daisysp::Oscillator::WAVE_SAW,
  square = daisysp::Oscillator::WAVE_SQUARE,
  tri = daisysp::Oscillator::WAVE_TRI,
  NUM_WAVEFORMS = 3
};

Switch waveform_button;
GPIO waveform_led;
int waveform = saw;

// Setup delay
constexpr size_t MAX_DELAY = 96000;
daisysp::DelayLine<float, MAX_DELAY> delay;

// Delay state
float delay_target = 12000.0f; 
float delay_smooth = 12000.0f;
float feedback = 0.35f;
float mix = 0.4f;

//Step timing (the clock)
float phase = 0.0f;

//Tempo smoothing
float bpm_target = 120.0f;
float bpm_smooth = 120.0f;
float steps_per_beat = 2.0f;

constexpr int NUM_STEPS = 8;

float step_freqs[NUM_STEPS] = {
  220.0f, 330.0f, 440.0f, 330.0f, 220.0f, 165.0f, 330.0f, 440.0f
};

int current_step = 0;

enum AdcChannel {
  tempo = 0,
  filter_cutoff,
  osc_mod,
  resonance_mod,
  NUM_ADC_CHANNELS
};

float osc_freq = 440.0f;

float bitcrush(float in, int bits) {
  float max_level = (1 << bits) - 1;
  float out = roundf(in * max_level) / max_level;
  return out;
}

int bitcrush_counter = 0;
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

void MyCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
  for (size_t i = 0; i < size; i++) {
    
    //Update the clock
    bpm_smooth += 0.001f * (bpm_target - bpm_smooth);
    
    // Convert BPM -> Hz
    float step_hz = (bpm_smooth / 60.0f) * steps_per_beat;
    float phase_inc = step_hz / hw.AudioSampleRate();

    //Advance phasor
    phase += phase_inc;

    if (phase >= 1.0f) {
      phase -= 1.0f;
      current_step = (current_step + 1) % NUM_STEPS;
      osc.SetFreq(step_freqs[current_step]);

      //update the osc modulator
      float detune = 0.5f;
      if (osc_mod_amount > 0.0f) {
        osc2.SetFreq(step_freqs[current_step] * 1.5 - detune);
      } else if (osc_mod_amount < 0.0f) {
        osc2.SetFreq(step_freqs[current_step] * (2.0f/3.0f) - detune);
      } else {
        osc2.SetFreq(step_freqs[current_step]);
      }
    }

    // Update the signal, and warm it up
    float sig = osc.Process();
    float osc_drive = 3.0f;
    sig = tanhf(sig * osc_drive);

    // Add in osc mudulator
    sig = sig + (osc2.Process() * fabs(osc_mod_amount));

    filter.SetFreq(cutoff);
    filter.SetRes(resonance);
    filter.Process(sig);
    float out_sig = filter.Low();

    if(bitcrush_enabled) {
      out_sig = bitcrush(out_sig, 4);
      out_sig = bitcrush_sample(out_sig, bitcrush_counter, 4);
    }

    // Scale 
    float filter_drive = 0.5f;
    out_sig *= filter_drive;

    if(delay_enabled) {
      // Simple distortion effect
      // out_sig = out_sig * 5.0f;
      // if(out_sig > 1.0f) out_sig = 1.0f;
      // if(out_sig < -1.0f) out_sig = -1.0f;

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
  // hw.StartLog();

  int sample_rate = hw.AudioSampleRate();

  // Init oscillator
  osc.Init(sample_rate);
  osc.SetWaveform(saw);
  osc.SetAmp(0.02f);

  osc2.Init(sample_rate);
  osc2.SetWaveform(daisysp::Oscillator::WAVE_SIN);
  osc2.SetAmp(0.08f);

  // Init filter
  filter.Init(sample_rate);

  // Init delay
  delay.Init();
  delay.SetDelay(delay_smooth);

  // Create an ADC Channel Config object
  AdcChannelConfig adc_config[NUM_ADC_CHANNELS];
  adc_config[tempo].InitSingle(A0);
  adc_config[filter_cutoff].InitSingle(A4);
	adc_config[osc_mod].InitSingle(A1);
	adc_config[resonance_mod].InitSingle(A2);
  // adc_config[waveform].InitSingle(A3);
  hw.adc.Init(adc_config, NUM_ADC_CHANNELS);
  hw.adc.Start();

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

  resonance_mod_knob.Init(
    hw.adc.GetPtr(resonance_mod),
    hw.AudioSampleRate(),
    true
  );

  // waveform_knob.Init(
  //   hw.adc.GetPtr(waveform),
  //   hw.AudioSampleRate(),
  //   true
  // );

  // Buttons
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

  // Map knob to frequency range
  tempo_param.Init(tempo_knob, 40.0f, 240.0f, Parameter::LOGARITHMIC);
  cutoff_param.Init(cutoff_knob, min_cutoff, max_cutoff, Parameter::LOGARITHMIC);
  resonance_param.Init(cutoff_knob, 0.1f, 0.9f, Parameter::LINEAR);
  osc_mod_param.Init(osc_mod_knob, -1.0f, 1.0f, Parameter::LINEAR);
  resonance_mod_param.Init(resonance_mod_knob, -0.5f, 0.5f, Parameter::LINEAR);
  // waveform_param.Init(waveform_knob, 0.0f, 3.0f, Parameter::LINEAR);

  // Start the audio with our callback function
  hw.StartAudio(MyCallback);


  while (1) {

    //update tempo
    tempo_knob.Process();
    double_tempo_button.Debounce();
    double_tempo_enabled = double_tempo_button.Pressed();
    if(double_tempo_enabled) {
      bpm_target = tempo_param.Process() * 2.0f;
    } else {
      bpm_target = tempo_param.Process();
    }
    double_tempo_led.Write(double_tempo_enabled);

    //Update waveform
    waveform_button.Debounce();
    waveform_led.Write(waveform_button.Pressed());
    if(waveform_button.RisingEdge()) {
      waveform = (waveform + 1) % NUM_WAVEFORMS;
      osc.SetWaveform(waveform);
      // osc2.SetWaveform(waveform);
    }

    //update filter cutoff
    cutoff_knob.Process();
    cutoff = cutoff_param.Process();
    resonance = resonance_param.Process();
    
    // update modulation amounts
    osc_mod_knob.Process();
    resonance_mod_knob.Process();
    osc_mod_amount = osc_mod_param.Process();
    resonance_mod_amount = resonance_mod_param.Process();
    
    // Check buttons
    delay_button.Debounce();
    delay_enabled = delay_button.Pressed();
    delay_led.Write(delay_enabled);

    bitcrush_button.Debounce();
    bitcrush_enabled = bitcrush_button.Pressed();
    bitcrush_led.Write(bitcrush_enabled);

    // In order to know that everything's working let's print that to a serial console:
    // hw.PrintLine("cut %f", val);
    // hw.PrintLine("res %f", val2);
    // System::Delay(100);

    System::Delay(1);
  }
}
