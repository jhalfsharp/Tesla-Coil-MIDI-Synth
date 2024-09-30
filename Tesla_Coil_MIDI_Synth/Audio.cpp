#include "Audio.h"
#include "Synth.h"
#include "USBAudio.h"

#include <Arduino.h>

namespace Audio {

// Audio processing mode
AudioMode audioMode = AM_OFF;

const char *audioModeNames[] = {
  "Off",
  "Binary",
  "Processed",
  "PWM"
};

// Sample period
uint32_t period = (F_CPU/NOM_SAMPLE_RATE) << PERIOD_ADJ_SPEED;

// Ring buffer of buffers
volatile Buffer bufs[NBUFS];
uint8_t readBuffer, writeBuffer;

// Empty buffer to insert when no data available
uint16_t zeroBuf[ZERO_BUF_SIZE];

// Flag to drop incoming buffers if we're really behind on playing
bool purgeBufs;

// DMA and stuff currently enabled
bool audioRunning;

// Last time we got new audio data
unsigned long lastAudioTimestamp;

// Last processed sample (used for AM_PROCESSED mode)
int32_t lastSample;

// Baseline (used for AM_PWM mode)
int32_t baseline;

// Return amount of filled buffers in ring buffer of buffers
uint8_t bufsFilled() {
  uint8_t diff = writeBuffer - readBuffer;
  if(diff > NBUFS)
    diff += NBUFS;
  return diff;
}

// Called from ISR when one buffer has been played
void setDMABuffer() {
  // Check amount of buffers available
  uint8_t filled = bufsFilled();

  // If there is some data, play it with DMA
  if(filled) {
    PDC_PWM->PERIPH_TNPR = (uint32_t)bufs[readBuffer].buf;
    PDC_PWM->PERIPH_TNCR = bufs[readBuffer].len;

    // Increment read buffer
    readBuffer++;
    if(readBuffer >= NBUFS)
      readBuffer = 0;
    filled--;
  }

  // Otherwise insert a bit of silence
  else {
    PDC_PWM->PERIPH_TNPR = (uint32_t)zeroBuf;
    PDC_PWM->PERIPH_TNCR = ZERO_BUF_SIZE;
  }

  // Update sample rate based on current buffer fill level
  if(filled > 1)
    period = max(period-1, (F_CPU/MAX_SAMPLE_RATE) << PERIOD_ADJ_SPEED);
  else if(filled == 0)
    period = min(period+1, (F_CPU/MIN_SAMPLE_RATE) << PERIOD_ADJ_SPEED);

  PWM->PWM_CH_NUM[0].PWM_CPRDUPD = period >> PERIOD_ADJ_SPEED;
}

void initAudio() {
  // Enable clock to PWM
  PMC->PMC_PCER1 = (1 << (ID_PWM-32));

  // Set up PWM
  PWM->PWM_SCM = PWM_SCM_SYNC0 // Enable channel 0 as a synchronous channel
   | PWM_SCM_UPDM_MODE2; // Update synchronous channels from PDC
  PWM->PWM_IDR1 = 0xFFFFFFFF; // Disable interrupts
  PWM->PWM_IDR2 = 0xFFFFFFFF;
  PWM->PWM_CH_NUM[0].PWM_CPRD = period >> PERIOD_ADJ_SPEED; // Set period to 48kHz
  PWM->PWM_CH_NUM[0].PWM_CCNT = 0; // Reset counter

  // (Prepare to) connect PWML0 to PA21B
  PIO->PIO_ABSR = PIN; // Select peripheral function B
  PIO->PIO_OER = PIN; // Fall back to low output when PWM not selected
  PIO->PIO_CODR = PIN;

  stopAudio();

  // Enable data transmission using PWM PDC
  PDC_PWM->PERIPH_PTCR = PERIPH_PTCR_TXTEN;

  // Allow interrupts from PWM
  NVIC->ISER[1] = (1 << (PWM_IRQn-32));
}

void startAudio() {
  // Disable PIO control, switch to peripheral function
  PIO->PIO_PDR = PIN;

  // Init buffer states
  readBuffer = 0;
  writeBuffer = 0;
  purgeBufs = false;
  lastSample = 0;
  baseline = 0;

  setDMABuffer();

  // Enable end of buffer interrupt
  PWM->PWM_IER2 = PWM_IER2_ENDTX;

  // Start PWM
  PWM->PWM_ENA = PWM_ENA_CHID0;

  audioRunning = true;
}

void stopAudio() {
  // Enable PIO control, setting output low
  PIO->PIO_PER = PIN;

  // Stop PWM
  PWM->PWM_DIS = PWM_DIS_CHID0;

  // Disable interrupt
  PWM->PWM_IDR2 = 0xFFFFFFFF;

  // Clear USB audio buffer
  char dummy;
  while(USBAudio.available())
    USBAudio.read(&dummy, 1);

  audioRunning = false;
}

void processAudio() {
  // Wait for data to be available
  if(USBAudio.available()) {
    lastAudioTimestamp = millis();

    // Make sure DMA is running
    if(!audioRunning)
      startAudio();

    // Temporary place for unprocessed USB audio samples
    static int16_t rxBuf[BUF_SIZE];

    uint32_t len = USBAudio.read(rxBuf, BUF_SIZE*2);
    len /= 2; // Two bytes per sample

    uint8_t filled = bufsFilled();

    // Stop catching up/purging if we have done enough
    if(purgeBufs && filled < 2)
      purgeBufs = false;

    // Fill a new buffer if there is space
    if(!purgeBufs && filled < NBUFS-2) {
      // Process the new samples
      for(uint32_t x = 0; x < len; x++)
        bufs[writeBuffer].buf[x] = processSample(rxBuf[x]);
      bufs[writeBuffer].len = len;

      // Increment buffer
      writeBuffer++;
      if(writeBuffer >= NBUFS)
        writeBuffer = 0;
    }

    // Start purging if there isn't enough space
    else purgeBufs = true;
  }

  // If audio hasn't been running for a while, stop
  else {
    if(audioRunning && millis() - lastAudioTimestamp > AUDIO_TIMEOUT)
      stopAudio();
  }
}

uint16_t processSample(int32_t in) {
  uint16_t ret;

  switch(audioMode) {
    // Hope you like square waves
    case AM_BINARY:
      ret = in > 0 ? 0xFFFF : 0;
      break;

    // TODO
    case AM_PROCESSED:
      ret = 0;
      break;

    // Duty cycle proportional to input
    case AM_PWM:
      // Clamp minimum input to zero
      if(in - baseline < 0)
        baseline = in;

      // Subtract away baseline
      in -= baseline;

      // Decay baseline to zero
      baseline = baseline*253/256;

      // Apply volume and make proportional to max PWM counter value
      ret = in * Synth::vol / 0x100 * (F_CPU/NOM_SAMPLE_RATE) / 0x10000;
      break;

    default:
      ret = 0;
      break;
  }

  lastSample = in;
  return ret;
}

}
