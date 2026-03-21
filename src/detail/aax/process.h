#pragma once

// AAX includes
#include "AAX.h"
#include "AAX_IMIDINode.h"

#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_ALG
#include AAX_ALIGN_FILE_END

class ClapAsAAX;

struct SAAX_Wrapper_PrivateData
{
  ClapAsAAX *wrapper;
};

// this is the struct that is used to communicate with the audio processing
// our private data pointer The odd thing is that we declare which AAX entity
// is at which offset, the host fills them in and passes them on processing

struct SAAX_Wrapper_AlgorithmicContext
{
  float **mAudioInputs = nullptr;   ///< Audio input buffers
  float **mAudioOutputs = nullptr;  ///< Audio output buffers, including any aux output stems.
  int32_t *mNumSamples =
      nullptr;  ///< Number of samples in each buffer.  Bounded as per \ref AAE_EAudioBufferLengthNative.
                // The exact value can vary from buffer to buffer.
  AAX_CTimestamp *mClock = nullptr;  ///< Pointer to the global running time clock.

  // TODO: check if this is necessary to keep in the context
  AAX_IMIDINode *mInputNode =
      nullptr;  ///< Buffered local MIDI input node. Used for incoming MIDI messages directed to the instrument.
  AAX_IMIDINode *mOutputNode =
      nullptr;  ///< Buffered local MIDI input node. Used for incoming MIDI messages directed to the instrument.
  AAX_IMIDINode *mGlobalNode =
      nullptr;  ///< Buffered global MIDI input node. Used for global events like beat updates in metronomes.
  AAX_IMIDINode *mTransportNode =
      nullptr;  ///< Transport MIDI node.  Used for querying the state of the MIDI transport.
  //  AAX_IMIDINode*              mAdditionalInputMIDINodes[kMaxAdditionalMIDINodes];  ///< List of additional input MIDI nodes, if your plugin needs them.

  SAAX_Wrapper_PrivateData *mPrivateData = nullptr;

  float **mMeters =
      nullptr;  ///< Array of meter taps.  One meter value should be entered per tap for each render call.

  int64_t *mCurrentStateNum = nullptr;  ///< State counter

  // perhaps we need more, later
};

#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_RESET
#include AAX_ALIGN_FILE_END

void AAX_CALLBACK AAXWrapper_AlgorithmProcessProc(
    SAAX_Wrapper_AlgorithmicContext *const inInstancesBegin[], const void *inInstancesEnd);

int32_t AAX_CALLBACK
AAXWrapper_inInstanceInitProc(const SAAX_Wrapper_AlgorithmicContext *inInstanceContextPtr,
                              AAX_EComponentInstanceInitAction inAction);

int32_t AAX_CALLBACK AAXWrapper_BackgroundProc();