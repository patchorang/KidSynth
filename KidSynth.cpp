#include "daisy_seed.h"

using namespace daisy;
using namespace daisy::seed;
#include "daisysp.h"

// Create out Daisy Seed Hardware object
DaisySeed hw;
daisysp::Oscillator osc;
daisysp::Svf filter;

AnalogControl tempo_knob;
Parameter tempo_param;
float cutoff = 1000.0f;
float resonance = 0.1f;

AnalogControl cutoff_knob;
Parameter cutoff_param;
Parameter resonance_param;

//Step timing (the clock)
float bpm = 120.0f;
float steps_per_beat = 2.0f;
float sample_rate = 48000.0f;

uint32_t samples_per_step = (60.0f / bpm) * sample_rate / steps_per_beat;	
uint32_t sample_counter;

constexpr int NUM_STEPS = 8;

float step_freqs[NUM_STEPS] = {
	220.0f, 330.0f, 440.0f, 330.0f, 220.0f, 165.0f, 330.0f, 440.0f
};

int current_step = 0;

enum AdcChannel {
	tempo = 0,
	filter_cutoff,
	NUM_ADC_CHANNELS
};

float osc_freq = 440.0f;

void MyCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
	for (size_t i = 0; i < size; i++) {
		
		//Update the clock
		sample_counter++;
		if(sample_counter >= samples_per_step) {
			sample_counter = 0;
			current_step = (current_step + 1) % NUM_STEPS;

			osc.SetFreq(step_freqs[current_step]);
		}

		// Update the signal
		float sig = osc.Process();

		// Update the filter
		filter.SetFreq(cutoff);
		filter.SetRes(resonance);
		filter.Process(sig);
		float filtered = filter.Low();

		out[0][i] = filtered;
		out[1][i] = filtered;
	}
}

int main(void) {
  // Initialize the Daisy Seed hardware
	hw.Configure();
  hw.Init();
	hw.StartLog();

	int sample_rate = hw.AudioSampleRate();
	// Init oscillator
	osc.Init(sample_rate);
	osc.SetWaveform(daisysp::Oscillator::WAVE_SAW);
	osc.SetAmp(0.05f);

	// Init filter
	filter.Init(sample_rate);

  // Create an ADC Channel Config object
  AdcChannelConfig adc_config[NUM_ADC_CHANNELS];
	adc_config[tempo].InitSingle(A0);
	adc_config[filter_cutoff].InitSingle(A4);
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

	// Map knob to frequency range
	tempo_param.Init(tempo_knob, 40.0f, 240.0f, Parameter::LOGARITHMIC);
	cutoff_param.Init(cutoff_knob, 40.0f, 10000.0f, Parameter::LOGARITHMIC);
	resonance_param.Init(cutoff_knob, 0.1f, 0.9f, Parameter::LINEAR);

	// Start the audio with our callback function
	hw.StartAudio(MyCallback);


	while (1) {
		//update osc frequency
		// freq_knob.Process();
		// float freq = freq_param.Process();
		// osc.SetFreq(freq);s
		tempo_knob.Process();
		float tempo = tempo_param.Process();
		if(tempo != bpm) { 
			bpm = tempo;
			samples_per_step = (60.0f / bpm) * sample_rate / steps_per_beat;	
		}

		//update filter cutoff
		cutoff_knob.Process();
		cutoff = cutoff_param.Process();
		resonance = resonance_param.Process();

    // In order to know that everything's working let's print that to a serial console:
    // hw.PrintLine("Frequency: %f", freq);
		// hw.PrintLine("Cutoff: %f", cutoff);
    // Wait half a second (500 milliseconds)
    //System::Delay(250);

		System::Delay(1);
	}
}