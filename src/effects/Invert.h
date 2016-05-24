/**********************************************************************

  Audacity: A Digital Audio Editor

  Invert.h

  Mark Phillips

  This class inverts the selected audio.

**********************************************************************/

#ifndef __AUDACITY_EFFECT_INVERT__
#define __AUDACITY_EFFECT_INVERT__

#include <wx/string.h>

#include "Effect.h"

#define INVERT_PLUGIN_SYMBOL XO("Invert")

class EffectInvert final : public Effect
{
public:
   EffectInvert();
   virtual ~EffectInvert();

   // IdentInterface implementation

   wxString GetSymbol() override;
   wxString GetDescription() override;

   // EffectIdentInterface implementation

   EffectType GetType() override;
   bool IsInteractive() override;

   // EffectClientInterface implementation

   int GetAudioInCount() override;
   int GetAudioOutCount() override;
   sampleCount ProcessBlock(float **inBlock, float **outBlock, sampleCount blockLen) override;
};

#endif
