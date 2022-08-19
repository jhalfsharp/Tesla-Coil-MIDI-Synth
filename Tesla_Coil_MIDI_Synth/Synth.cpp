#include "Synth.h"
#include "Voice.h"
#include "MIDI.h"

#include <Arduino.h>
#include <limits.h>

namespace Synth {

// Volume control
uint8_t vol = DEFAULT_VOL;

uint8_t eLookup[256];
int8_t sinLookup[256];

void updateWidth(uint8_t chan, uint32_t pulseWidth);
void updatePeriod(uint8_t chan, uint32_t period);

void initSynth() {
  // Precompute exponential decay thing
  const float scale = exp(-EXP_CRUNCH);
  for(int x=0; x<256; x++)
    eLookup[x] = (exp(-x*EXP_CRUNCH/255.0)-scale)/(1-scale)*255;

  // Precompute sine
  for(int x=0; x<256; x++)
    sinLookup[x] = sin(2*PI*x/256.0)*127;
}

void updateSynth() {
  // Prevent from running while voice settings are being updated
  if(Voice::voicesUpdating) {
    Voice::voicesUpdating = 2;
    return;
  }
  
  // Update voices
  int totalEnv = 0;
  for(int x=0; x<NVOICES; x++) {
    Voice::Voice &voice = Voice::voices[x];
    
    if(voice.active) {
      unsigned long ms = millis();
  
      // Process ADSR/pulse width
      uint16_t env = 255;
      if(voice.midiChannel == MIDI::CHANNEL_FX || voice.midiChannel == MIDI::CHANNEL_ARP || voice.midiChannel == MIDI::CHANNEL_DRUM) {
        uint32_t localA = MIDI::attack;
        uint32_t localD = MIDI::decay;
        uint8_t localS = MIDI::sustain;
        uint32_t localR = MIDI::release;
        if(voice.drum) {
          localA = voice.drum->a;
          localR = voice.drum->r;
        }
  
        // Increment stage if needed
        bool nextStage = false;
        unsigned long dt = ms-voice.adsrTimestamp;
        if(voice.adsrStage == 0) {
          if(dt > localA) {
            nextStage = true;
            if(voice.midiChannel == MIDI::CHANNEL_DRUM) {
              voice.adsrStage = 2; // Skip D and S for drum
              voice.lastEnv = 255;
            }
          }
        } else if(voice.adsrStage == 1) {
          if(dt > localD) nextStage = true;
        } else if(voice.adsrStage == 3) {
          if(dt > localR) {
            voice.active = false;
            nextStage = true;
            env = 0;
            if(voice.midiChannel == MIDI::CHANNEL_ARP) memset(voice.arpNoteEndTimestamps, 0, sizeof(voice.arpNoteEndTimestamps));
          }
        }
        if(voice.adsrStage < 3 && !voice.midiNoteDown) {
          voice.adsrStage = 2;
          nextStage = true;
        }
        if(nextStage) {
          voice.adsrStage++;
          voice.adsrTimestamp = ms;
          dt = 0;
        }
  
        // Compute envelope
        if(voice.adsrStage == 0) {
          if(localA == 0) localA = 1;
          env = 255-eLookup[(uint64_t)dt*255/localA];
        } else if(voice.adsrStage == 1) {
          if(localD == 0) localD = 1;
          env = (255-localS)*(uint32_t)eLookup[(uint64_t)dt*255/localD]/255+localS;
        } else if(voice.adsrStage == 2) {
          env = localS;
        } else if(voice.adsrStage == 3) {
          if(localR == 0) localR = 1;
          env = voice.lastEnv*(uint32_t)eLookup[(uint64_t)dt*255/localR]/255;
        }

        // Keep track of env right before release so it releases from the right point
        if(voice.adsrStage < 3) voice.lastEnv = env;

        // Apply MIDI note velocity
        env = env*voice.midiVel*2/255;
      } else {
        if(voice.midiNoteDown) env = voice.midiVel*2;
        else {
          voice.active = false;
          env = 0;
        }
      }
      
      // Process drum/arp
      float note = 100;
      if(voice.midiChannel == MIDI::CHANNEL_DRUM) {
        note = voice.drum->baseNote;
        note *= env/255.0*voice.drum->envMod+1;
        note *= ((float)rand()/RAND_MAX*2-1)*voice.drum->noiseMod+1;
      } else if(voice.midiChannel == MIDI::CHANNEL_ARP) {
        // Check if all the notes have been released
        if(voice.midiNoteDown) {
          voice.midiNoteDown = false;
          for(int y=0; y<MAX_ARP_NOTES; y++)
            if((int64_t)voice.arpNoteEndTimestamps[y] - (int64_t)ARPEGGIO_LINGER > ms) {
              voice.midiNoteDown = true;
              break;
            }
        }
        // If all keys have been released, lock all remaining notes on
        if(!voice.midiNoteDown) {
          for(int y=0; y<MAX_ARP_NOTES; y++)
            if(voice.arpNoteEndTimestamps[y] > ms) voice.arpNoteEndTimestamps[y] = ULONG_MAX;
        }
        // Go to next note
        if(ms-voice.arpTimestamp > MIDI::arpeggioPeriod) {
          for(int y=0; y<MAX_ARP_NOTES; y++) {
            voice.arpNotesIndex++;
            if(voice.arpNotesIndex >= MAX_ARP_NOTES) voice.arpNotesIndex = 0;
            if(voice.arpNoteEndTimestamps[voice.arpNotesIndex] > ms) break;
          }
          voice.arpTimestamp = ms;
        }
        note = MIDI::midi2freq[voice.arpNotes[voice.arpNotesIndex]];
      } else note = MIDI::midi2freq[voice.midiNote];

      // Process pitch bend
      note *= 1+(PITCH_BEND_RANGE-1)*voice.midiPB/0x2000;
  
      // Process tremolo and vibrato
      if(voice.midiChannel == MIDI::CHANNEL_FX || voice.midiChannel == MIDI::CHANNEL_ARP) {
        unsigned long dt = ms-voice.noteDownTimestamp;
        
        uint32_t lookupIndex = (uint64_t)dt*255/MIDI::tremoloDelay;
        uint8_t tremoloAmount;
        if(lookupIndex > 255) tremoloAmount = 255;
        else tremoloAmount = lookupIndex;
  
        lookupIndex = (uint64_t)dt*255/MIDI::vibratoDelay;
        uint8_t vibratoAmount;
        if(lookupIndex > 255) vibratoAmount = 255;
        else vibratoAmount = lookupIndex;
  
        int8_t tremoloOscillate = sinLookup[((uint64_t)dt*255/MIDI::tremoloPeriod)&0xFF];
        int8_t vibratoOscillate = sinLookup[((uint64_t)dt*255/MIDI::vibratoPeriod)&0xFF];
  
        env *= (int32_t)MIDI::tremoloDepth*tremoloAmount*tremoloOscillate/8258175.0+1;
        note *= (int32_t)MIDI::vibratoDepth*vibratoAmount*vibratoOscillate/8258175.0+1;
      }

      voice.period = F_CPU/2/note;
      
      // Save env in pulseWidth; will be overwritten later to the correct pulse width
      voice.pulseWidth = env;

      // Keep track of the total amount of stuff playing
      totalEnv += env;
    }
  }

  // Compute how much to reduce the pulse width based on how much stuff is currently playing to make it more intelligible
#ifdef AUTODUCK
  uint16_t duck = (255*255) / max(totalEnv-255, 255);
#else
  static const uint16_t duck = 255;
#endif

  // Iterate through voices again to update the timers
  for(int x=0; x<NVOICES; x++) {
    Voice::Voice &voice = Voice::voices[x];
    
    if(voice.active) {
      // Make duty cycle correspond to envelope
      uint16_t env = voice.pulseWidth;
#ifdef ABSOLUTE_PULSE_WIDTH
      uint64_t maxWidth = MAX_WIDTH*3/4*vol/255;
#else
      uint64_t maxWidth = voice.period*3/4*vol/255; // Limit to 75% duty cycle
      if(maxWidth > MAX_WIDTH) maxWidth = MAX_WIDTH;
#endif
      voice.pulseWidth = maxWidth*env*duck/65025;

      if((int32_t)voice.period-(int32_t)voice.pulseWidth < MIN_OFF_TIME) voice.pulseWidth = voice.period - MIN_OFF_TIME;

      // Update timer
      updatePeriod(x, voice.period);
      updateWidth(x, voice.pulseWidth);
    } else { // Note is not active
      updateWidth(x, 0);
    }
  }

  // Reset WDT
  WDT->WDT_CR = WDT_CR_KEY(0xA5) | WDT_CR_WDRSTT;
}

// Update pulse width of a timer
inline __attribute__((always_inline)) void updateWidth(uint8_t chan, uint32_t pulseWidth) {
  const Voice::VoiceConfig &vc = Voice::voiceConfigs[chan];
  if(vc.timerab) {
    // If we decrease the compare value below the counter value, it will never equal it to set the pin low
    //   and the pin will stay high for an entire period (bad).
    // Include some margin since counter will keep going as the code runs
    if(vc.channel->TC_RB > vc.channel->TC_CV && pulseWidth < vc.channel->TC_CV+10) {
      uint32_t temp = vc.channel->TC_CV; // save count value
      vc.channel->TC_CCR = TC_CCR_SWTRG; // trigger the timer to reset it back to 0 and set pin low
      vc.channel->TC_CV = temp; // put counter value back
    }
    vc.channel->TC_RB = pulseWidth; // update pulse width
    if(pulseWidth < MIN_WIDTH) {
      vc.channel->TC_CMR &= ~TC_CMR_BCPC_SET; // disable output if pulse width is too small
      vc.channel->TC_CCR = TC_CCR_SWTRG; // re-trigger the timer
    } else vc.channel->TC_CMR |= TC_CMR_BCPC_SET;
  } else {
    if(vc.channel->TC_RA > vc.channel->TC_CV && pulseWidth < vc.channel->TC_CV+10) {
      uint32_t temp = vc.channel->TC_CV;
      vc.channel->TC_CCR = TC_CCR_SWTRG;
      vc.channel->TC_CV = temp;
    }
    vc.channel->TC_RA = pulseWidth;
    if(pulseWidth < MIN_WIDTH) {
      vc.channel->TC_CMR &= ~TC_CMR_ACPC_SET;
      vc.channel->TC_CCR = TC_CCR_SWTRG;
    } else vc.channel->TC_CMR |= TC_CMR_ACPC_SET;
  }
}

// Update frequency of a timer
inline __attribute__((always_inline)) void updatePeriod(uint8_t chan, uint32_t period) {
  const Voice::VoiceConfig &vc = Voice::voiceConfigs[chan];
  vc.channel->TC_RC = period;
  if(vc.channel->TC_CV > period) { // Reset so counter stays below the period (otherwise would get long pulses)
    if(vc.timerab) {
      // Temporarily make software trigger set the pin instead of reset in order to behave the same as an RC compare
      if(vc.channel->TC_CMR & TC_CMR_BCPC_SET) { // Only do this if the output is enabled
        vc.channel->TC_CMR = (vc.channel->TC_CMR & ~(0b11<<TC_CMR_BSWTRG_Pos)) | TC_CMR_BSWTRG_SET;
        vc.channel->TC_CCR = TC_CCR_SWTRG;
        vc.channel->TC_CMR = (vc.channel->TC_CMR & ~(0b11<<TC_CMR_BSWTRG_Pos)) | TC_CMR_BSWTRG_CLEAR;
      }
    } else {
      if(vc.channel->TC_CMR & TC_CMR_ACPC_SET) {
        vc.channel->TC_CMR = (vc.channel->TC_CMR & ~(0b11<<TC_CMR_ASWTRG_Pos)) | TC_CMR_ASWTRG_SET;
        vc.channel->TC_CCR = TC_CCR_SWTRG;
        vc.channel->TC_CMR = (vc.channel->TC_CMR & ~(0b11<<TC_CMR_ASWTRG_Pos)) | TC_CMR_ASWTRG_CLEAR;
      }
    }
  }
}

}
