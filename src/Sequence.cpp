/**********************************************************************

  Audacity: A Digital Audio Editor

  Sequence.cpp

  Dominic Mazzoni

*******************************************************************//**
\file Sequence.cpp
\brief Implements classes Sequence and SeqBlock.

*//****************************************************************//**

\class Sequence
\brief A WaveTrack contains WaveClip(s).
   A WaveClip contains a Sequence. A Sequence is primarily an
   interface to an array of SeqBlock instances, corresponding to
   the audio BlockFiles on disk.
   Contrast with RingBuffer.

*//****************************************************************//**

\class SeqBlock
\brief Data structure containing pointer to a BlockFile and
   a start time. Element of a BlockArray.

*//*******************************************************************/


#include "Audacity.h"
#include "Sequence.h"

#include <algorithm>
#include <float.h>
#include <math.h>

#include <wx/dynarray.h>
#include <wx/intl.h>
#include <wx/filefn.h>
#include <wx/ffile.h>
#include <wx/log.h>

#include "AudacityException.h"

#include "BlockFile.h"
#include "blockfile/ODDecodeBlockFile.h"
#include "DirManager.h"

#include "blockfile/SimpleBlockFile.h"
#include "blockfile/SilentBlockFile.h"

#include "InconsistencyException.h"

size_t Sequence::sMaxDiskBlockSize = 1048576;

// Sequence methods
Sequence::Sequence(const std::shared_ptr<DirManager> &projDirManager, sampleFormat format)
   : mDirManager(projDirManager)
   , mSampleFormat(format)
   , mMinSamples(sMaxDiskBlockSize / SAMPLE_SIZE(mSampleFormat) / 2)
   , mMaxSamples(mMinSamples * 2)
{
}

// essentially a copy constructor - but you must pass in the
// current project's DirManager, because we might be copying
// from one project to another
Sequence::Sequence(const Sequence &orig, const std::shared_ptr<DirManager> &projDirManager)
   : mDirManager(projDirManager)
   , mSampleFormat(orig.mSampleFormat)
   , mMinSamples(orig.mMinSamples)
   , mMaxSamples(orig.mMaxSamples)
{
   Paste(0, &orig);
}

Sequence::~Sequence()
{
}

size_t Sequence::GetMaxBlockSize() const
{
   return mMaxSamples;
}

size_t Sequence::GetIdealBlockSize() const
{
   return mMaxSamples;
}

bool Sequence::Lock()
{
   for (unsigned int i = 0; i < mBlock.size(); i++)
      mBlock[i].f->Lock();

   return true;
}

bool Sequence::CloseLock()
{
   for (unsigned int i = 0; i < mBlock.size(); i++)
      mBlock[i].f->CloseLock();

   return true;
}

bool Sequence::Unlock()
{
   for (unsigned int i = 0; i < mBlock.size(); i++)
      mBlock[i].f->Unlock();

   return true;
}

sampleFormat Sequence::GetSampleFormat() const
{
   return mSampleFormat;
}

/*
bool Sequence::SetSampleFormat(sampleFormat format)
{
   if (mBlock.size() > 0 || mNumSamples > 0)
      return false;

   mSampleFormat = format;
   return true;
}
*/

bool Sequence::ConvertToSampleFormat(sampleFormat format)
// STRONG-GUARANTEE
{
   if (format == mSampleFormat)
      // no change
      return false;

   if (mBlock.size() == 0)
   {
      mSampleFormat = format;
      return true;
   }

   const sampleFormat oldFormat = mSampleFormat;
   mSampleFormat = format;

   const auto oldMinSamples = mMinSamples, oldMaxSamples = mMaxSamples;
   // These are the same calculations as in the constructor.
   mMinSamples = sMaxDiskBlockSize / SAMPLE_SIZE(mSampleFormat) / 2;
   mMaxSamples = mMinSamples * 2;

   bool bSuccess = false;
   auto cleanup = finally( [&] {
      if (!bSuccess) {
         // Conversion failed. Revert these member vars.
         mSampleFormat = oldFormat;
         mMaxSamples = oldMaxSamples;
         mMinSamples = oldMinSamples;
      }
   } );

   BlockArray newBlockArray;
   // Use the ratio of old to NEW mMaxSamples to make a reasonable guess
   // at allocation.
   newBlockArray.reserve
      (1 + mBlock.size() * ((float)oldMaxSamples / (float)mMaxSamples));

   {
      SampleBuffer bufferOld(oldMaxSamples, oldFormat);
      SampleBuffer bufferNew(oldMaxSamples, format);

      for (size_t i = 0, nn = mBlock.size(); i < nn; i++)
      {
         SeqBlock &oldSeqBlock = mBlock[i];
         const auto &oldBlockFile = oldSeqBlock.f;
         const auto len = oldBlockFile->GetLength();
         Read(bufferOld.ptr(), oldFormat, oldSeqBlock, 0, len, true);

         CopySamples(bufferOld.ptr(), oldFormat, bufferNew.ptr(), format, len);

         // Note this fix for http://bugzilla.audacityteam.org/show_bug.cgi?id=451,
         // using Blockify, allows (len < mMinSamples).
         // This will happen consistently when going from more bytes per sample to fewer...
         // This will create a block that's smaller than mMinSamples, which
         // shouldn't be allowed, but we agreed it's okay for now.
         //vvv ANSWER-ME: Does this cause any bugs, or failures on write, elsewhere?
         //    If so, need to special-case (len < mMinSamples) and start combining data
         //    from the old blocks... Oh no!

         // Using Blockify will handle the cases where len > the NEW mMaxSamples. Previous code did not.
         const auto blockstart = oldSeqBlock.start;
         const unsigned prevSize = newBlockArray.size();
         Blockify(*mDirManager, mMaxSamples, mSampleFormat,
                  newBlockArray, blockstart, bufferNew.ptr(), len);
      }
   }

   // Invalidate all the old, non-aliased block files.
   // Aliased files will be converted at save, per comment above.

   // Commit the changes to block file array
   CommitChangesIfConsistent
      (newBlockArray, mNumSamples, wxT("Sequence::ConvertToSampleFormat()"));

   // Commit the other changes
   bSuccess = true;

   return true;
}

std::pair<float, float> Sequence::GetMinMax(
   sampleCount start, sampleCount len, bool mayThrow) const
{
   if (len == 0 || mBlock.size() == 0) {
      return {
         0.f,
         // FLT_MAX?  So it doesn't look like a spurious '0' to a caller?

         0.f
         // -FLT_MAX?  So it doesn't look like a spurious '0' to a caller?
      };
   }

   float min = FLT_MAX;
   float max = -FLT_MAX;

   unsigned int block0 = FindBlock(start);
   unsigned int block1 = FindBlock(start + len - 1);

   // First calculate the min/max of the blocks in the middle of this region;
   // this is very fast because we have the min/max of every entire block
   // already in memory.

   for (unsigned b = block0 + 1; b < block1; ++b) {
      auto results = mBlock[b].f->GetMinMaxRMS(mayThrow);

      if (results.min < min)
         min = results.min;
      if (results.max > max)
         max = results.max;
   }

   // Now we take the first and last blocks into account, noting that the
   // selection may only partly overlap these blocks.  If the overall min/max
   // of either of these blocks is within min...max, then we can ignore them.
   // If not, we need read some samples and summaries from disk.
   {
      const SeqBlock &theBlock = mBlock[block0];
      const auto &theFile = theBlock.f;
      auto results = theFile->GetMinMaxRMS(mayThrow);

      if (results.min < min || results.max > max) {
         // start lies within theBlock:
         auto s0 = ( start - theBlock.start ).as_size_t();
         const auto maxl0 = (
            // start lies within theBlock:
            theBlock.start + theFile->GetLength() - start
         ).as_size_t();
         wxASSERT(maxl0 <= mMaxSamples); // Vaughan, 2011-10-19
         const auto l0 = limitSampleBufferSize ( maxl0, len );

         results = theFile->GetMinMaxRMS(s0, l0, mayThrow);
         if (results.min < min)
            min = results.min;
         if (results.max > max)
            max = results.max;
      }
   }

   if (block1 > block0)
   {
      const SeqBlock &theBlock = mBlock[block1];
      const auto &theFile = theBlock.f;
      auto results = theFile->GetMinMaxRMS(mayThrow);

      if (results.min < min || results.max > max) {

         // start + len - 1 lies in theBlock:
         const auto l0 = ( start + len - theBlock.start ).as_size_t();
         wxASSERT(l0 <= mMaxSamples); // Vaughan, 2011-10-19

         results = theFile->GetMinMaxRMS(0, l0, mayThrow);
         if (results.min < min)
            min = results.min;
         if (results.max > max)
            max = results.max;
      }
   }

   return { min, max };
}

float Sequence::GetRMS(sampleCount start, sampleCount len, bool mayThrow) const
{
   // len is the number of samples that we want the rms of.
   // it may be longer than a block, and the code is carefully set up to handle that.
   if (len == 0 || mBlock.size() == 0)
      return 0.f;

   double sumsq = 0.0;
   sampleCount length = 0; // this is the cumulative length of the bits we have the ms of so far, and should end up == len

   unsigned int block0 = FindBlock(start);
   unsigned int block1 = FindBlock(start + len - 1);

   // First calculate the rms of the blocks in the middle of this region;
   // this is very fast because we have the rms of every entire block
   // already in memory.
   for (unsigned b = block0 + 1; b < block1; b++) {
      const SeqBlock &theBlock = mBlock[b];
      const auto &theFile = theBlock.f;
      auto results = theFile->GetMinMaxRMS(mayThrow);

      const auto fileLen = theFile->GetLength();
      const auto blockRMS = results.RMS;
      sumsq += blockRMS * blockRMS * fileLen;
      length += fileLen;
   }

   // Now we take the first and last blocks into account, noting that the
   // selection may only partly overlap these blocks.
   // If not, we need read some samples and summaries from disk.
   {
      const SeqBlock &theBlock = mBlock[block0];
      const auto &theFile = theBlock.f;
      // start lies within theBlock
      auto s0 = ( start - theBlock.start ).as_size_t();
      // start lies within theBlock
      const auto maxl0 =
         (theBlock.start + theFile->GetLength() - start).as_size_t();
      wxASSERT(maxl0 <= mMaxSamples); // Vaughan, 2011-10-19
      const auto l0 = limitSampleBufferSize( maxl0, len );

      auto results = theFile->GetMinMaxRMS(s0, l0, mayThrow);
      const auto partialRMS = results.RMS;
      sumsq += partialRMS * partialRMS * l0;
      length += l0;
   }

   if (block1 > block0) {
      const SeqBlock &theBlock = mBlock[block1];
      const auto &theFile = theBlock.f;

      // start + len - 1 lies within theBlock
      const auto l0 = ( start + len - theBlock.start ).as_size_t();
      wxASSERT(l0 <= mMaxSamples); // PRL: I think Vaughan missed this

      auto results = theFile->GetMinMaxRMS(0, l0, mayThrow);
      const auto partialRMS = results.RMS;
      sumsq += partialRMS * partialRMS * l0;
      length += l0;
   }

   // PRL: catch bugs like 1320:
   wxASSERT(length == len);

   return sqrt(sumsq / length.as_double() );
}

std::unique_ptr<Sequence> Sequence::Copy(sampleCount s0, sampleCount s1) const
{
   auto dest = std::make_unique<Sequence>(mDirManager, mSampleFormat);
   if (s0 >= s1 || s0 >= mNumSamples || s1 < 0) {
      return dest;
   }

   int numBlocks = mBlock.size();

   int b0 = FindBlock(s0);
   const int b1 = FindBlock(s1 - 1);
   wxASSERT(b0 >= 0);
   wxASSERT(b0 < numBlocks);
   wxASSERT(b1 < numBlocks);
   wxUnusedVar(numBlocks);
   wxASSERT(b0 <= b1);

   dest->mBlock.reserve(b1 - b0 + 1);

   SampleBuffer buffer(mMaxSamples, mSampleFormat);

   int blocklen;

   // Do the first block

   const SeqBlock &block0 = mBlock[b0];
   if (s0 != block0.start) {
      const auto &file = block0.f;
      // Nonnegative result is length of block0 or less:
      blocklen =
         ( std::min(s1, block0.start + file->GetLength()) - s0 ).as_size_t();
      wxASSERT(file->IsAlias() || (blocklen <= mMaxSamples)); // Vaughan, 2012-02-29
      Get(b0, buffer.ptr(), mSampleFormat, s0, blocklen, true);

      dest->Append(buffer.ptr(), mSampleFormat, blocklen);
   }
   else
      --b0;

   // If there are blocks in the middle, copy the blockfiles directly
   for (int bb = b0 + 1; bb < b1; ++bb)
      AppendBlock(*dest->mDirManager, dest->mBlock, dest->mNumSamples, mBlock[bb]);
      // Increase ref count or duplicate file

   // Do the last block
   if (b1 > b0) {
      const SeqBlock &block = mBlock[b1];
      const auto &file = block.f;
      // s1 is within block:
      blocklen = (s1 - block.start).as_size_t();
      wxASSERT(file->IsAlias() || (blocklen <= mMaxSamples)); // Vaughan, 2012-02-29
      if (blocklen < file->GetLength()) {
         Get(b1, buffer.ptr(), mSampleFormat, block.start, blocklen, true);
         dest->Append(buffer.ptr(), mSampleFormat, blocklen);
      }
      else
         // Special case, copy exactly
         AppendBlock(*dest->mDirManager, dest->mBlock, dest->mNumSamples, block);
         // Increase ref count or duplicate file
   }

   dest->ConsistencyCheck(wxT("Sequence::Copy()"));

   return dest;
}

namespace {
   inline bool Overflows(double numSamples)
   {
      return numSamples > wxLL(9223372036854775807);
   }
}

void Sequence::Paste(sampleCount s, const Sequence *src)
// STRONG-GUARANTEE
{
   if ((s < 0) || (s > mNumSamples))
   {
      wxLogError(
         wxT("Sequence::Paste: sampleCount s %s is < 0 or > mNumSamples %s)."),
         // PRL:  Why bother with Internat when the above is just wxT?
         Internat::ToString(s.as_double(), 0).c_str(),
         Internat::ToString(mNumSamples.as_double(), 0).c_str());
      THROW_INCONSISTENCY_EXCEPTION;
   }

   // Quick check to make sure that it doesn't overflow
   if (Overflows((mNumSamples.as_double()) + (src->mNumSamples.as_double())))
   {
      wxLogError(
         wxT("Sequence::Paste: mNumSamples %s + src->mNumSamples %s would overflow."),
         // PRL:  Why bother with Internat when the above is just wxT?
         Internat::ToString(mNumSamples.as_double(), 0).c_str(),
         Internat::ToString(src->mNumSamples.as_double(), 0).c_str());
      THROW_INCONSISTENCY_EXCEPTION;
   }

   if (src->mSampleFormat != mSampleFormat)
   {
      wxLogError(
         wxT("Sequence::Paste: Sample format to be pasted, %s, does not match destination format, %s."),
         GetSampleFormatStr(src->mSampleFormat), GetSampleFormatStr(src->mSampleFormat));
      THROW_INCONSISTENCY_EXCEPTION;
   }

   const BlockArray &srcBlock = src->mBlock;
   auto addedLen = src->mNumSamples;
   const unsigned int srcNumBlocks = srcBlock.size();
   auto sampleSize = SAMPLE_SIZE(mSampleFormat);

   if (addedLen == 0 || srcNumBlocks == 0)
      return;

   const size_t numBlocks = mBlock.size();

   if (numBlocks == 0 ||
       (s == mNumSamples && mBlock.back().f->GetLength() >= mMinSamples)) {
      // Special case: this track is currently empty, or it's safe to append
      // onto the end because the current last block is longer than the
      // minimum size

      // Build and swap a copy so there is a strong exception safety guarantee
      BlockArray newBlock{ mBlock };
      sampleCount samples = mNumSamples;
      for (unsigned int i = 0; i < srcNumBlocks; i++)
         // AppendBlock may throw for limited disk space, if pasting from
         // one project into another.
         AppendBlock(*mDirManager, newBlock, samples, srcBlock[i]);
         // Increase ref count or duplicate file

      CommitChangesIfConsistent
         (newBlock, samples, wxT("Paste branch one"));
      return;
   }

   const int b = (s == mNumSamples) ? mBlock.size() - 1 : FindBlock(s);
   wxASSERT((b >= 0) && (b < (int)numBlocks));
   SeqBlock *const pBlock = &mBlock[b];
   const auto length = pBlock->f->GetLength();
   const auto largerBlockLen = addedLen + length;
   // PRL: when insertion point is the first sample of a block,
   // and the following test fails, perhaps we could test
   // whether coalescence with the previous block is possible.
   if (largerBlockLen <= mMaxSamples) {
      // Special case: we can fit all of the NEW samples inside of
      // one block!

      SeqBlock &block = *pBlock;
      // largerBlockLen is not more than mMaxSamples...
      SampleBuffer buffer(largerBlockLen.as_size_t(), mSampleFormat);

      // ...and addedLen is not more than largerBlockLen
      auto sAddedLen = addedLen.as_size_t();
      // s lies within block:
      auto splitPoint = ( s - block.start ).as_size_t();
      Read(buffer.ptr(), mSampleFormat, block, 0, splitPoint, true);
      src->Get(0, buffer.ptr() + splitPoint*sampleSize,
               mSampleFormat, 0, sAddedLen, true);
      Read(buffer.ptr() + (splitPoint + sAddedLen) * sampleSize,
           mSampleFormat, block,
           splitPoint, length - splitPoint, true);

      auto file =
         mDirManager->NewSimpleBlockFile(
            // largerBlockLen is not more than mMaxSamples...
            buffer.ptr(), largerBlockLen.as_size_t(), mSampleFormat);

      // Don't make a duplicate array.  We can still give STRONG-GUARANTEE
      // if we modify only one block in place.

      // use NOFAIL-GUARANTEE in remaining steps
      block.f = file;

      for (unsigned int i = b + 1; i < numBlocks; i++)
         mBlock[i].start += addedLen;

      mNumSamples += addedLen;

      // This consistency check won't throw, it asserts.
      // Proof that we kept consistency is not hard.
      ConsistencyCheck(wxT("Paste branch two"), false);
      return;
   }

   // Case three: if we are inserting four or fewer blocks,
   // it's simplest to just lump all the data together
   // into one big block along with the split block,
   // then resplit it all
   BlockArray newBlock;
   newBlock.reserve(numBlocks + srcNumBlocks + 2);
   newBlock.insert(newBlock.end(), mBlock.begin(), mBlock.begin() + b);

   SeqBlock &splitBlock = mBlock[b];
   auto splitLen = splitBlock.f->GetLength();
   // s lies within splitBlock
   auto splitPoint = ( s - splitBlock.start ).as_size_t();

   unsigned int i;
   if (srcNumBlocks <= 4) {

      // addedLen is at most four times maximum block size
      auto sAddedLen = addedLen.as_size_t();
      const auto sum = splitLen + sAddedLen;

      SampleBuffer sumBuffer(sum, mSampleFormat);
      Read(sumBuffer.ptr(), mSampleFormat, splitBlock, 0, splitPoint, true);
      src->Get(0, sumBuffer.ptr() + splitPoint * sampleSize,
               mSampleFormat,
               0, sAddedLen, true);
      Read(sumBuffer.ptr() + (splitPoint + sAddedLen) * sampleSize, mSampleFormat,
           splitBlock, splitPoint,
           splitLen - splitPoint, true);

      Blockify(*mDirManager, mMaxSamples, mSampleFormat,
               newBlock, splitBlock.start, sumBuffer.ptr(), sum);
   } else {

      // The final case is that we're inserting at least five blocks.
      // We divide these into three groups: the first two get merged
      // with the first half of the split block, the middle ones get
      // copied in as is, and the last two get merged with the last
      // half of the split block.

      const auto srcFirstTwoLen =
          srcBlock[0].f->GetLength() + srcBlock[1].f->GetLength();
      const auto leftLen = splitPoint + srcFirstTwoLen;

      const SeqBlock &penultimate = srcBlock[srcNumBlocks - 2];
      const auto srcLastTwoLen =
         penultimate.f->GetLength() +
         srcBlock[srcNumBlocks - 1].f->GetLength();
      const auto rightSplit = splitBlock.f->GetLength() - splitPoint;
      const auto rightLen = rightSplit + srcLastTwoLen;

      SampleBuffer sampleBuffer(std::max(leftLen, rightLen), mSampleFormat);

      Read(sampleBuffer.ptr(), mSampleFormat, splitBlock, 0, splitPoint, true);
      src->Get(0, sampleBuffer.ptr() + splitPoint*sampleSize,
         mSampleFormat, 0, srcFirstTwoLen, true);

      Blockify(*mDirManager, mMaxSamples, mSampleFormat,
               newBlock, splitBlock.start, sampleBuffer.ptr(), leftLen);

      for (i = 2; i < srcNumBlocks - 2; i++) {
         const SeqBlock &block = srcBlock[i];
         auto file = mDirManager->CopyBlockFile(block.f);
         // We can assume file is not null
         newBlock.push_back(SeqBlock(file, block.start + s));
      }

      auto lastStart = penultimate.start;
      src->Get(srcNumBlocks - 2, sampleBuffer.ptr(), mSampleFormat,
               lastStart, srcLastTwoLen, true);
      Read(sampleBuffer.ptr() + srcLastTwoLen * sampleSize, mSampleFormat,
           splitBlock, splitPoint, rightSplit, true);

      Blockify(*mDirManager, mMaxSamples, mSampleFormat,
               newBlock, s + lastStart, sampleBuffer.ptr(), rightLen);
   }

   // Copy remaining blocks to NEW block array and
   // swap the NEW block array in for the old
   for (i = b + 1; i < numBlocks; i++)
      newBlock.push_back(mBlock[i].Plus(addedLen));

   CommitChangesIfConsistent
      (newBlock, mNumSamples + addedLen, wxT("Paste branch three"));
}

void Sequence::SetSilence(sampleCount s0, sampleCount len)
// STRONG-GUARANTEE
{
   SetSamples(NULL, mSampleFormat, s0, len);
}

void Sequence::InsertSilence(sampleCount s0, sampleCount len)
// STRONG-GUARANTEE
{
   // Quick check to make sure that it doesn't overflow
   if (Overflows((mNumSamples.as_double()) + (len.as_double())))
      THROW_INCONSISTENCY_EXCEPTION;

   if (len <= 0)
      return;

   // Create a NEW track containing as much silence as we
   // need to insert, and then call Paste to do the insertion.
   // We make use of a SilentBlockFile, which takes up no
   // space on disk.

   Sequence sTrack(mDirManager, mSampleFormat);

   auto idealSamples = GetIdealBlockSize();

   sampleCount pos = 0;

   // Could nBlocks overflow a size_t?  Not very likely.  You need perhaps
   // 2 ^ 52 samples which is over 3000 years at 44.1 kHz.
   auto nBlocks = (len + idealSamples - 1) / idealSamples;
   sTrack.mBlock.reserve(nBlocks.as_size_t());

   BlockFilePtr silentFile {};
   if (len >= idealSamples)
      silentFile = make_blockfile<SilentBlockFile>(idealSamples);
   while (len >= idealSamples) {
      sTrack.mBlock.push_back(SeqBlock(silentFile, pos));

      pos += idealSamples;
      len -= idealSamples;
   }
   if (len != 0) {
      sTrack.mBlock.push_back(SeqBlock(
         // len is not more than idealSamples:
         make_blockfile<SilentBlockFile>( len.as_size_t() ), pos));
      pos += len;
   }

   sTrack.mNumSamples = pos;

   // use STRONG-GUARANTEE
   Paste(s0, &sTrack);
}

void Sequence::AppendAlias(const wxString &fullPath,
                           sampleCount start,
                           size_t len, int channel, bool useOD)
// STRONG-GUARANTEE
{
   // Quick check to make sure that it doesn't overflow
   if (Overflows((mNumSamples.as_double()) + ((double)len)))
      THROW_INCONSISTENCY_EXCEPTION;

   SeqBlock newBlock(
      useOD?
         mDirManager->NewODAliasBlockFile(fullPath, start, len, channel):
         mDirManager->NewAliasBlockFile(fullPath, start, len, channel),
      mNumSamples
   );
   mBlock.push_back(newBlock);
   mNumSamples += len;
}

void Sequence::AppendCoded(const wxString &fName, sampleCount start,
                            size_t len, int channel, int decodeType)
// STRONG-GUARANTEE
{
   // Quick check to make sure that it doesn't overflow
   if (Overflows((mNumSamples.as_double()) + ((double)len)))
      THROW_INCONSISTENCY_EXCEPTION;

   SeqBlock newBlock(
      mDirManager->NewODDecodeBlockFile(fName, start, len, channel, decodeType),
      mNumSamples
   );
   mBlock.push_back(newBlock);
   mNumSamples += len;
}

void Sequence::AppendBlock
   (DirManager &mDirManager,
    BlockArray &mBlock, sampleCount &mNumSamples, const SeqBlock &b)
{
   // Quick check to make sure that it doesn't overflow
   if (Overflows((mNumSamples.as_double()) + ((double)b.f->GetLength())))
      THROW_INCONSISTENCY_EXCEPTION;

   SeqBlock newBlock(
      mDirManager.CopyBlockFile(b.f), // Bump ref count if not locked, else copy
      mNumSamples
   );
   // We can assume newBlock.f is not null

   mBlock.push_back(newBlock);
   mNumSamples += newBlock.f->GetLength();

   // Don't do a consistency check here because this
   // function gets called in an inner loop.
}

///gets an int with OD flags so that we can determine which ODTasks should be run on this track after save/open, etc.
unsigned int Sequence::GetODFlags()
{
   unsigned int ret = 0;
   for (unsigned int i = 0; i < mBlock.size(); i++) {
      const auto &file = mBlock[i].f;
      if(!file->IsDataAvailable())
         ret |= (static_cast< ODDecodeBlockFile * >( &*file ))->GetDecodeType();
      else if(!file->IsSummaryAvailable())
         ret |= ODTask::eODPCMSummary;
   }
   return ret;
}

sampleCount Sequence::GetBlockStart(sampleCount position) const
{
   int b = FindBlock(position);
   return mBlock[b].start;
}

size_t Sequence::GetBestBlockSize(sampleCount start) const
{
   // This method returns a nice number of samples you should try to grab in
   // one big chunk in order to land on a block boundary, based on the starting
   // sample.  The value returned will always be nonzero and will be no larger
   // than the value of GetMaxBlockSize()

   if (start < 0 || start >= mNumSamples)
      return mMaxSamples;

   int b = FindBlock(start);
   int numBlocks = mBlock.size();

   const SeqBlock &block = mBlock[b];
   // start is in block:
   auto result = (block.start + block.f->GetLength() - start).as_size_t();

   decltype(result) length;
   while(result < mMinSamples && b+1<numBlocks &&
         ((length = mBlock[b+1].f->GetLength()) + result) <= mMaxSamples) {
      b++;
      result += length;
   }

   wxASSERT(result > 0 && result <= mMaxSamples);

   return result;
}

bool Sequence::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   /* handle waveblock tag and its attributes */
   if (!wxStrcmp(tag, wxT("waveblock"))) {
      SeqBlock wb;

      // loop through attrs, which is a null-terminated list of
      // attribute-value pairs
      while(*attrs) {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         long long nValue = 0;

         if (!value)
            break;

         // Both these attributes have non-negative integer counts of samples, so
         // we can test & convert here, making sure that values > 2^31 are OK
         // because long clips will need them.
         const wxString strValue = value;
         if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
         {
            mErrorOpening = true;
            wxLogWarning(
               wxT("   Sequence has bad %s attribute value, %s, that should be a positive integer."),
               attr, strValue.c_str());
            return false;
         }

         if (!wxStrcmp(attr, wxT("start")))
            wb.start = nValue;

         // Vaughan, 2011-10-10: I don't think we ever write a "len" attribute for "waveblock" tag,
         // so I think this is actually legacy code, or something intended, but not completed.
         // Anyway, might as well leave this code in, especially now that it has the check
         // against mMaxSamples.
         if (!wxStrcmp(attr, wxT("len")))
         {
            // mMaxSamples should already have been set by calls to the "sequence" clause below.
            // The check intended here was already done in DirManager::HandleXMLTag(), where
            // it let the block be built, then checked against mMaxSamples, and deleted the block
            // if the size of the block is bigger than mMaxSamples.
            if (nValue > mMaxSamples)
            {
               mErrorOpening = true;
               return false;
            }
            mDirManager->SetLoadingBlockLength(nValue);
         }
      } // while

      mBlock.push_back(wb);
      mDirManager->SetLoadingTarget(&mBlock, mBlock.size() - 1);

      return true;
   }

   /* handle sequence tag and its attributes */
   if (!wxStrcmp(tag, wxT("sequence"))) {
      while(*attrs) {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
            break;

         long long nValue = 0;

         const wxString strValue = value;	// promote string, we need this for all

         if (!wxStrcmp(attr, wxT("maxsamples")))
         {
            // This attribute is a sample count, so can be 64bit
            if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
            {
               mErrorOpening = true;
               return false;
            }
            // Dominic, 12/10/2006:
            //    Let's check that maxsamples is >= 1024 and <= 64 * 1024 * 1024
            //    - that's a pretty wide range of reasonable values.
            if ((nValue < 1024) || (nValue > 64 * 1024 * 1024))
            {
               mErrorOpening = true;
               return false;
            }

            // nValue is now safe for size_t
            mMaxSamples = nValue;

            // PRL:  Is the following really okay?  DirManager might be shared across projects!
            // PRL:  Yes, because it only affects DirManager's behavior in opening the project.
            mDirManager->SetLoadingMaxSamples(mMaxSamples);
         }
         else if (!wxStrcmp(attr, wxT("sampleformat")))
         {
            // This attribute is a sample format, normal int
            long fValue;
            if (!XMLValueChecker::IsGoodInt(strValue) || !strValue.ToLong(&fValue) || (fValue < 0) || !XMLValueChecker::IsValidSampleFormat(fValue))
            {
               mErrorOpening = true;
               return false;
            }
            mSampleFormat = (sampleFormat)fValue;
         }
         else if (!wxStrcmp(attr, wxT("numsamples")))
         {
            // This attribute is a sample count, so can be 64bit
            if (!XMLValueChecker::IsGoodInt64(strValue) || !strValue.ToLongLong(&nValue) || (nValue < 0))
            {
               mErrorOpening = true;
               return false;
            }
            mNumSamples = nValue;
         }
      } // while

      //// Both mMaxSamples and mSampleFormat should have been set.
      //// Check that mMaxSamples is right for mSampleFormat, using the calculations from the constructor.
      //if ((mMinSamples != sMaxDiskBlockSize / SAMPLE_SIZE(mSampleFormat) / 2) ||
      //      (mMaxSamples != mMinSamples * 2))
      //{
      //   mErrorOpening = true;
      //   return false;
      //}

      return true;
   }

   return false;
}

void Sequence::HandleXMLEndTag(const wxChar *tag)
{
   if (wxStrcmp(tag, wxT("sequence")) != 0)
      return;

   // Make sure that the sequence is valid.
   // First, replace missing blockfiles with SilentBlockFiles
   for (unsigned b = 0, nn = mBlock.size(); b < nn; b++) {
      SeqBlock &block = mBlock[b];
      if (!block.f) {
         sampleCount len;

         if (b < nn - 1)
            len = mBlock[b+1].start - block.start;
         else
            len = mNumSamples - block.start;

         if (len > mMaxSamples)
         {
            // This could be why the blockfile failed, so limit
            // the silent replacement to mMaxSamples.
            wxLogWarning(
               wxT("   Sequence has missing block file with length %s > mMaxSamples %s.\n      Setting length to mMaxSamples. This will likely cause some block files to be considered orphans."),
               // PRL:  Why bother with Internat when the above is just wxT?
               Internat::ToString(len.as_double(), 0).c_str(),
               Internat::ToString((double)mMaxSamples, 0).c_str());
            len = mMaxSamples;
         }
         // len is at most mMaxSamples:
         block.f = make_blockfile<SilentBlockFile>( len.as_size_t() );
         wxLogWarning(
            wxT("Gap detected in project file. Replacing missing block file with silence."));
         mErrorOpening = true;
      }
   }

   // Next, make sure that start times and lengths are consistent
   sampleCount numSamples = 0;
   for (unsigned b = 0, nn = mBlock.size(); b < nn;  b++) {
      SeqBlock &block = mBlock[b];
      if (block.start != numSamples) {
         wxString sFileAndExtension = block.f->GetFileName().name.GetFullName();
         if (sFileAndExtension.IsEmpty())
            sFileAndExtension = wxT("(replaced with silence)");
         else
            sFileAndExtension = wxT("\"") + sFileAndExtension + wxT("\"");
         wxLogWarning(
            wxT("Gap detected in project file.\n")
            wxT("   Start (%s) for block file %s is not one sample past end of previous block (%s).\n")
            wxT("   Moving start so blocks are contiguous."),
            // PRL:  Why bother with Internat when the above is just wxT?
            Internat::ToString(block.start.as_double(), 0).c_str(),
            sFileAndExtension.c_str(),
            Internat::ToString(numSamples.as_double(), 0).c_str());
         block.start = numSamples;
         mErrorOpening = true;
      }
      numSamples += block.f->GetLength();
   }
   if (mNumSamples != numSamples) {
      wxLogWarning(
         wxT("Gap detected in project file. Correcting sequence sample count from %s to %s."),
         // PRL:  Why bother with Internat when the above is just wxT?
         Internat::ToString(mNumSamples.as_double(), 0).c_str(),
         Internat::ToString(numSamples.as_double(), 0).c_str());
      mNumSamples = numSamples;
      mErrorOpening = true;
   }
}

XMLTagHandler *Sequence::HandleXMLChild(const wxChar *tag)
{
   if (!wxStrcmp(tag, wxT("waveblock")))
      return this;
   else {
      mDirManager->SetLoadingFormat(mSampleFormat);
      return mDirManager.get();
   }
}

// Throws exceptions rather than reporting errors.
void Sequence::WriteXML(XMLWriter &xmlFile) const
// may throw
{
   unsigned int b;

   xmlFile.StartTag(wxT("sequence"));

   xmlFile.WriteAttr(wxT("maxsamples"), mMaxSamples);
   xmlFile.WriteAttr(wxT("sampleformat"), mSampleFormat);
   xmlFile.WriteAttr(wxT("numsamples"), mNumSamples.as_long_long() );

   for (b = 0; b < mBlock.size(); b++) {
      const SeqBlock &bb = mBlock[b];

      // See http://bugzilla.audacityteam.org/show_bug.cgi?id=451.
      // Also, don't check against mMaxSamples for AliasBlockFiles, because if you convert sample format,
      // mMaxSample gets changed to match the format, but the number of samples in the aliased file
      // has not changed (because sample format conversion was not actually done in the aliased file).
      if (!bb.f->IsAlias() && (bb.f->GetLength() > mMaxSamples))
      {
         // PRL:  Bill observed this error.  Not sure how it was caused.
         // I have added code in ConsistencyCheck that should abort the
         // editing operation that caused this, not fixing
         // the problem but moving the point of detection earlier if we
         // find a reproducible case.
         wxString sMsg =
            wxString::Format(
               _("Sequence has block file exceeding maximum %s samples per block.\nTruncating to this maximum length."),
               Internat::ToString(((wxLongLong)mMaxSamples).ToDouble(), 0).c_str());
         wxMessageBox(sMsg, _("Warning - Truncating Overlong Block File"), wxICON_EXCLAMATION | wxOK);
         wxLogWarning(sMsg);
         bb.f->SetLength(mMaxSamples);
      }

      xmlFile.StartTag(wxT("waveblock"));
      xmlFile.WriteAttr(wxT("start"), bb.start.as_long_long() );

      bb.f->SaveXML(xmlFile);

      xmlFile.EndTag(wxT("waveblock"));
   }

   xmlFile.EndTag(wxT("sequence"));
}

int Sequence::FindBlock(sampleCount pos) const
{
   wxASSERT(pos >= 0 && pos < mNumSamples);

   if (pos == 0)
      return 0;

   int numBlocks = mBlock.size();

   size_t lo = 0, hi = numBlocks, guess;
   sampleCount loSamples = 0, hiSamples = mNumSamples;

   while (true) {
      //this is not a binary search, but a
      //dictionary search where we guess something smarter than the binary division
      //of the unsearched area, since samples are usually proportional to block file number.
      const double frac = (pos - loSamples).as_double() /
         (hiSamples - loSamples).as_double();
      guess = std::min(hi - 1, lo + size_t(frac * (hi - lo)));
      const SeqBlock &block = mBlock[guess];

      wxASSERT(block.f->GetLength() > 0);
      wxASSERT(lo <= guess && guess < hi && lo < hi);

      if (pos < block.start) {
         wxASSERT(lo != guess);
         hi = guess;
         hiSamples = block.start;
      }
      else {
         const sampleCount nextStart = block.start + block.f->GetLength();
         if (pos < nextStart)
            break;
         else {
            wxASSERT(guess < hi - 1);
            lo = guess + 1;
            loSamples = nextStart;
         }
      }
   }

   const int rval = guess;
   wxASSERT(rval >= 0 && rval < numBlocks &&
            pos >= mBlock[rval].start &&
            pos < mBlock[rval].start + mBlock[rval].f->GetLength());

   return rval;
}

//static
bool Sequence::Read(samplePtr buffer, sampleFormat format,
                    const SeqBlock &b, size_t blockRelativeStart, size_t len,
                    bool mayThrow)
{
   const auto &f = b.f;

   wxASSERT(blockRelativeStart + len <= f->GetLength());

   // Either throws, or of !mayThrow, tells how many were really read
   auto result = f->ReadData(buffer, format, blockRelativeStart, len, mayThrow);

   if (result != len)
   {
      wxLogWarning(wxT("Expected to read %ld samples, got %d samples."),
                   len, result);
      return false;
   }

   return true;
}

bool Sequence::Get(samplePtr buffer, sampleFormat format,
   sampleCount start, size_t len, bool mayThrow) const
{
   if (start == mNumSamples) {
      return len == 0;
   }

   if (start < 0 || start > mNumSamples ||
       start + len > mNumSamples) {
      if (mayThrow)
         THROW_INCONSISTENCY_EXCEPTION;
      ClearSamples( buffer, floatSample, 0, len );
      return false;
   }
   int b = FindBlock(start);

   return Get(b, buffer, format, start, len, mayThrow);
}

bool Sequence::Get(int b, samplePtr buffer, sampleFormat format,
   sampleCount start, size_t len, bool mayThrow) const
{
   bool result = true;
   while (len) {
      const SeqBlock &block = mBlock[b];
      // start is in block
      const auto bstart = (start - block.start).as_size_t();
      // bstart is not more than block length
      const auto blen = std::min(len, block.f->GetLength() - bstart);

      if (! Read(buffer, format, block, bstart, blen, mayThrow) )
         result = false;

      len -= blen;
      buffer += (blen * SAMPLE_SIZE(format));
      b++;
      start += blen;
   }
   return result;
}

// Pass NULL to set silence
void Sequence::SetSamples(samplePtr buffer, sampleFormat format,
                   sampleCount start, sampleCount len)
// STRONG-GUARANTEE
{
   if (start < 0 || start >= mNumSamples ||
       start + len > mNumSamples)
      THROW_INCONSISTENCY_EXCEPTION;

   SampleBuffer scratch(mMaxSamples, mSampleFormat);

   SampleBuffer temp;
   if (buffer && format != mSampleFormat) {
      const auto size = limitSampleBufferSize( mMaxSamples, len );
      temp.Allocate(size, mSampleFormat);
   }

   int b = FindBlock(start);
   BlockArray newBlock;
   std::copy( mBlock.begin(), mBlock.begin() + b, std::back_inserter(newBlock) );

   while (len != 0) {
      newBlock.push_back( mBlock[b] );
      SeqBlock &block = newBlock.back();
      // start is within block
      const auto bstart = ( start - block.start ).as_size_t();
      const auto fileLength = block.f->GetLength();
      const auto blen = limitSampleBufferSize( fileLength - bstart, len );

      samplePtr useBuffer = buffer;
      if (buffer && format != mSampleFormat)
      {
         // To do: remove the extra movement.
         CopySamples(buffer, format, temp.ptr(), mSampleFormat, blen);
         useBuffer = temp.ptr();
      }

      // We don't ever write to an existing block; to support Undo,
      // we copy the old block entirely into memory, dereference it,
      // make the change, and then write the NEW block to disk.

      if (!(fileLength <= mMaxSamples &&
            bstart + blen <= fileLength))
         THROW_INCONSISTENCY_EXCEPTION;

      if ( bstart > 0 || blen < fileLength ) {
         Read(scratch.ptr(), mSampleFormat, block, 0, fileLength, true);

         if (useBuffer) {
            auto sampleSize = SAMPLE_SIZE(mSampleFormat);
            memcpy(scratch.ptr() +
                   bstart * sampleSize, useBuffer, blen * sampleSize);
         }
         else
            ClearSamples(scratch.ptr(), mSampleFormat, bstart, blen);

         block.f = mDirManager->NewSimpleBlockFile(
            scratch.ptr(), fileLength, mSampleFormat);
      }
      else {
         // Avoid reading the disk when the replacement is total
         if (useBuffer)
            block.f = mDirManager->NewSimpleBlockFile(
               useBuffer, fileLength, mSampleFormat);
         else
            block.f = make_blockfile<SilentBlockFile>(fileLength);
      }

      if( buffer )
         buffer += (blen * SAMPLE_SIZE(format));

      len -= blen;
      start += blen;
      b++;
   }

   std::copy( mBlock.begin() + b, mBlock.end(), std::back_inserter(newBlock) );

   CommitChangesIfConsistent( newBlock, mNumSamples, wxT("SetSamples") );
}

namespace {

struct MinMaxSumsq
{
   MinMaxSumsq(const float *pv, int count, int divisor)
   {
      min = FLT_MAX, max = -FLT_MAX, sumsq = 0.0f;
      while (count--) {
         float v;
         switch (divisor) {
         default:
         case 1:
            // array holds samples
            v = *pv++;
            if (v < min)
               min = v;
            if (v > max)
               max = v;
            sumsq += v * v;
            break;
         case 256:
         case 65536:
            // array holds triples of min, max, and rms values
            v = *pv++;
            if (v < min)
               min = v;
            v = *pv++;
            if (v > max)
               max = v;
            v = *pv++;
            sumsq += v * v;
            break;
         }
      }
   }

   float min;
   float max;
   float sumsq;
};

}

bool Sequence::GetWaveDisplay(float *min, float *max, float *rms, int* bl,
                              size_t len, const sampleCount *where) const
{
   wxASSERT(len > 0);
   const auto s0 = std::max(sampleCount(0), where[0]);
   if (s0 >= mNumSamples)
      // None of the samples asked for are in range. Abandon.
      return false;

   // In case where[len - 1] == where[len], raise the limit by one,
   // so we load at least one pixel for column len - 1
   // ... unless the mNumSamples ceiling applies, and then there are other defenses
   const auto s1 =
      std::min(mNumSamples, std::max(1 + where[len - 1], where[len]));
   Floats temp{ mMaxSamples };

   decltype(len) pixel = 0;

   auto srcX = s0;
   decltype(srcX) nextSrcX = 0;
   int lastRmsDenom = 0;
   int lastDivisor = 0;
   auto whereNow = std::min(s1 - 1, where[0]);
   decltype(whereNow) whereNext = 0;
   // Loop over block files, opening and reading and closing each
   // not more than once
   unsigned nBlocks = mBlock.size();
   const unsigned int block0 = FindBlock(s0);
   for (unsigned int b = block0; b < nBlocks; ++b) {
      if (b > block0)
         srcX = nextSrcX;
      if (srcX >= s1)
         break;

      // Find the range of sample values for this block that
      // are in the display.
      const SeqBlock &seqBlock = mBlock[b];
      const auto start = seqBlock.start;
      nextSrcX = std::min(s1, start + seqBlock.f->GetLength());

      // The column for pixel p covers samples from
      // where[p] up to but excluding where[p + 1].

      // Find the range of pixels covered by the current block file
      // (Their starting samples covered by it, to be exact)
      decltype(len) nextPixel;
      if (nextSrcX >= s1)
         // last pass
         nextPixel = len;
      else {
         nextPixel = pixel;
         // Taking min with s1 - 1, here and elsewhere, is another defense
         // to be sure the last pixel column gets at least one sample
         while (nextPixel < len &&
                (whereNext = std::min(s1 - 1, where[nextPixel])) < nextSrcX)
            ++nextPixel;
      }
      if (nextPixel == pixel)
         // The entire block's samples fall within one pixel column.
         // Either it's a rare odd block at the end, or else,
         // we must be really zoomed out!
         // Omit the entire block's contents from min/max/rms
         // calculation, which is not correct, but correctness might not
         // be worth the compute time if this happens every pixel
         // column. -- PRL
         continue;
      if (nextPixel == len)
         whereNext = s1;

      // Decide the summary level
      const double samplesPerPixel =
         (whereNext - whereNow).as_double() / (nextPixel - pixel);
      const int divisor =
           (samplesPerPixel >= 65536) ? 65536
         : (samplesPerPixel >= 256) ? 256
         : 1;

      int blockStatus = b;

      // How many samples or triples are needed?

      const size_t startPosition =
         // srcX and start are in the same block
         std::max(sampleCount(0), (srcX - start) / divisor).as_size_t();
      const size_t inclusiveEndPosition =
         // nextSrcX - 1 and start are in the same block
         std::min((sampleCount(mMaxSamples) / divisor) - 1,
                  (nextSrcX - 1 - start) / divisor).as_size_t();
      const auto num = 1 + inclusiveEndPosition - startPosition;
      if (num <= 0) {
         // What?  There was a zero length block file?
         wxASSERT(false);
         // Do some defense against this case anyway
         while (pixel < nextPixel) {
            min[pixel] = max[pixel] = rms[pixel] = 0;
            bl[pixel] = blockStatus;//MC
            ++pixel;
         }
         continue;
      }

      // Read from the block file or its summary
      switch (divisor) {
      default:
      case 1:
         // Read samples
         // no-throw for display operations!
         Read((samplePtr)temp.get(), floatSample, seqBlock, startPosition, num, false);
         break;
      case 256:
         // Read triples
         //check to see if summary data has been computed
         if (seqBlock.f->IsSummaryAvailable())
            // Ignore the return value.
            // This function fills with zeroes if read fails
            seqBlock.f->Read256(temp.get(), startPosition, num);
         else
            //otherwise, mark the display as not yet computed
            blockStatus = -1 - b;
         break;
      case 65536:
         // Read triples
         //check to see if summary data has been computed
         if (seqBlock.f->IsSummaryAvailable())
            // Ignore the return value.
            // This function fills with zeroes if read fails
            seqBlock.f->Read64K(temp.get(), startPosition, num);
         else
            //otherwise, mark the display as not yet computed
            blockStatus = -1 - b;
         break;
      }
      
      auto filePosition = startPosition;

      // The previous pixel column might straddle blocks.
      // If so, impute some of the data to it.
      if (b > block0 && pixel > 0) {
         // whereNow and start are in the same block
         auto midPosition = ((whereNow - start) / divisor).as_size_t();
         int diff(midPosition - filePosition);
         if (diff > 0) {
            MinMaxSumsq values(temp.get(), diff, divisor);
            const int lastPixel = pixel - 1;
            float &lastMin = min[lastPixel];
            lastMin = std::min(lastMin, values.min);
            float &lastMax = max[lastPixel];
            lastMax = std::max(lastMax, values.max);
            float &lastRms = rms[lastPixel];
            int lastNumSamples = lastRmsDenom * lastDivisor;
            lastRms = sqrt(
               (lastRms * lastRms * lastNumSamples + values.sumsq * divisor) /
               (lastNumSamples + diff * divisor)
            );

            filePosition = midPosition;
         }
      }

      // Loop over file positions
      int rmsDenom = 0;
      for (; filePosition <= inclusiveEndPosition;) {
         // Find range of pixel columns for this file position
         // (normally just one, but maybe more when zoomed very close)
         // and the range of positions for those columns
         // (normally one or more, for that one column)
         auto pixelX = pixel + 1;
         decltype(filePosition) positionX = 0;
         while (pixelX < nextPixel &&
            filePosition ==
               (positionX = (
                  // s1 - 1 or where[pixelX] and start are in the same block
                  (std::min(s1 - 1, where[pixelX]) - start) / divisor).as_size_t() )
         )
            ++pixelX;
         if (pixelX >= nextPixel)
            positionX = 1 + inclusiveEndPosition;

         // Find results to assign
         rmsDenom = (positionX - filePosition);
         wxASSERT(rmsDenom > 0);
         const float *const pv =
            temp.get() + (filePosition - startPosition) * (divisor == 1 ? 1 : 3);
         MinMaxSumsq values(pv, rmsDenom, divisor);

         // Assign results
         std::fill(&min[pixel], &min[pixelX], values.min);
         std::fill(&max[pixel], &max[pixelX], values.max);
         std::fill(&bl[pixel], &bl[pixelX], blockStatus);
         std::fill(&rms[pixel], &rms[pixelX], (float)sqrt(values.sumsq / rmsDenom));

         pixel = pixelX;
         filePosition = positionX;
      }

      wxASSERT(pixel == nextPixel);
      whereNow = whereNext;
      pixel = nextPixel;
      lastDivisor = divisor;
      lastRmsDenom = rmsDenom;
   } // for each block file

   wxASSERT(pixel == len);

   return true;
}

size_t Sequence::GetIdealAppendLen() const
{
   int numBlocks = mBlock.size();
   const auto max = GetMaxBlockSize();

   if (numBlocks == 0)
      return max;

   const auto lastBlockLen = mBlock.back().f->GetLength();
   if (lastBlockLen == max)
      return max;
   else
      return max - lastBlockLen;
}

void Sequence::Append(samplePtr buffer, sampleFormat format,
                      size_t len, XMLWriter* blockFileLog /*=NULL*/)
// STRONG-GUARANTEE
{
   if (len == 0)
      return;

   // Quick check to make sure that it doesn't overflow
   if (Overflows(mNumSamples.as_double() + ((double)len)))
      THROW_INCONSISTENCY_EXCEPTION;

   BlockArray newBlock;
   sampleCount newNumSamples = mNumSamples;

   // If the last block is not full, we need to add samples to it
   int numBlocks = mBlock.size();
   SeqBlock *pLastBlock;
   decltype(pLastBlock->f->GetLength()) length;
   SampleBuffer buffer2(mMaxSamples, mSampleFormat);
   bool replaceLast = false;
   if (numBlocks > 0 &&
       (length =
        (pLastBlock = &mBlock.back())->f->GetLength()) < mMinSamples) {
      // Enlarge a sub-minimum block at the end
      const SeqBlock &lastBlock = *pLastBlock;
      const auto addLen = std::min(mMaxSamples - length, len);

      Read(buffer2.ptr(), mSampleFormat, lastBlock, 0, length, true);

      CopySamples(buffer,
                  format,
                  buffer2.ptr() + length * SAMPLE_SIZE(mSampleFormat),
                  mSampleFormat,
                  addLen);

      const auto newLastBlockLen = length + addLen;

      SeqBlock newLastBlock(
         mDirManager->NewSimpleBlockFile(
            buffer2.ptr(), newLastBlockLen, mSampleFormat,
            blockFileLog != NULL
         ),
         lastBlock.start
      );

      if (blockFileLog)
         // shouldn't throw, because XMLWriter is not XMLFileWriter
         static_cast< SimpleBlockFile * >( &*newLastBlock.f )
            ->SaveXML( *blockFileLog );

      newBlock.push_back( newLastBlock );

      len -= addLen;
      newNumSamples += addLen;
      buffer += addLen * SAMPLE_SIZE(format);

      replaceLast = true;
   }
   // Append the rest as NEW blocks
   while (len) {
      const auto idealSamples = GetIdealBlockSize();
      const auto addedLen = std::min(idealSamples, len);
      BlockFilePtr pFile;
      if (format == mSampleFormat) {
         pFile = mDirManager->NewSimpleBlockFile(
            buffer, addedLen, mSampleFormat, blockFileLog != NULL);
      }
      else {
         CopySamples(buffer, format, buffer2.ptr(), mSampleFormat, addedLen);
         pFile = mDirManager->NewSimpleBlockFile(
            buffer2.ptr(), addedLen, mSampleFormat, blockFileLog != NULL);
      }

      if (blockFileLog)
         // shouldn't throw, because XMLWriter is not XMLFileWriter
         static_cast< SimpleBlockFile * >( &*pFile )->SaveXML( *blockFileLog );

      newBlock.push_back(SeqBlock(pFile, newNumSamples));

      buffer += addedLen * SAMPLE_SIZE(format);
      newNumSamples += addedLen;
      len -= addedLen;
   }

   AppendBlocksIfConsistent(newBlock, replaceLast,
                            newNumSamples, wxT("Append"));

// JKC: During generate we use Append again and again.
// If generating a long sequence this test would give O(n^2)
// performance - not good!
#ifdef VERY_SLOW_CHECKING
   ConsistencyCheck(wxT("Append"));
#endif
}

void Sequence::Blockify
   (DirManager &mDirManager, size_t mMaxSamples, sampleFormat mSampleFormat,
    BlockArray &list, sampleCount start, samplePtr buffer, size_t len)
{
   if (len <= 0)
      return;
   auto num = (len + (mMaxSamples - 1)) / mMaxSamples;
   list.reserve(list.size() + num);

   for (decltype(num) i = 0; i < num; i++) {
      SeqBlock b;

      const auto offset = i * len / num;
      b.start = start + offset;
      int newLen = ((i + 1) * len / num) - offset;
      samplePtr bufStart = buffer + (offset * SAMPLE_SIZE(mSampleFormat));

      b.f = mDirManager.NewSimpleBlockFile(bufStart, newLen, mSampleFormat);

      list.push_back(b);
   }
}

void Sequence::Delete(sampleCount start, sampleCount len)
// STRONG-GUARANTEE
{
   if (len == 0)
      return;

   if (len < 0 || start < 0 || start >= mNumSamples)
      THROW_INCONSISTENCY_EXCEPTION;

   //TODO: add a ref-deref mechanism to SeqBlock/BlockArray so we don't have to make this a critical section.
   //On-demand threads iterate over the mBlocks and the GUI thread deletes them, so for now put a mutex here over
   //both functions,
   DeleteUpdateMutexLocker locker(*this);

   const unsigned int numBlocks = mBlock.size();

   const unsigned int b0 = FindBlock(start);
   unsigned int b1 = FindBlock(start + len - 1);

   auto sampleSize = SAMPLE_SIZE(mSampleFormat);

   SeqBlock *pBlock;
   decltype(pBlock->f->GetLength()) length;

   // One buffer for reuse in various branches here
   SampleBuffer scratch;
   // The maximum size that will ever be needed
   const auto scratchSize = mMaxSamples + mMinSamples;

   // Special case: if the samples to DELETE are all within a single
   // block and the resulting length is not too small, perform the
   // deletion within this block:
   if (b0 == b1 &&
       (length = (pBlock = &mBlock[b0])->f->GetLength()) - len >= mMinSamples) {
      SeqBlock &b = *pBlock;
      // start is within block
      auto pos = ( start - b.start ).as_size_t();
      wxASSERT(len < length);
      // len must be less than length
      // because start + len - 1 is also in the block...
      auto newLen = ( length - len ).as_size_t();

      scratch.Allocate(scratchSize, mSampleFormat);

      Read(scratch.ptr(), mSampleFormat, b, 0, pos, true);
      Read(scratch.ptr() + (pos * sampleSize), mSampleFormat,
           b,
           // ... and therefore pos + len
           // is not more than the length of the block
           ( pos + len ).as_size_t(), newLen - pos, true);

      auto newFile =
          mDirManager->NewSimpleBlockFile(scratch.ptr(), newLen, mSampleFormat);

      // Don't make a duplicate array.  We can still give STRONG-GUARANTEE
      // if we modify only one block in place.

      // use NOFAIL-GUARANTEE in remaining steps

      b.f = newFile;

      for (unsigned int j = b0 + 1; j < numBlocks; j++)
         mBlock[j].start -= len;

      mNumSamples -= len;

      // This consistency check won't throw, it asserts.
      // Proof that we kept consistency is not hard.
      ConsistencyCheck(wxT("Delete - branch one"), false);
      return;
   }

   // Create a NEW array of blocks
   BlockArray newBlock;
   newBlock.reserve(numBlocks - (b1 - b0) + 2);

   // Copy the blocks before the deletion point over to
   // the NEW array
   newBlock.insert(newBlock.end(), mBlock.begin(), mBlock.begin() + b0);
   unsigned int i;

   // First grab the samples in block b0 before the deletion point
   // into preBuffer.  If this is enough samples for its own block,
   // or if this would be the first block in the array, write it out.
   // Otherwise combine it with the previous block (splitting them
   // 50/50 if necessary).
   const SeqBlock &preBlock = mBlock[b0];
   // start is within preBlock
   auto preBufferLen = ( start - preBlock.start ).as_size_t();
   if (preBufferLen) {
      if (preBufferLen >= mMinSamples || b0 == 0) {
         if (!scratch.ptr())
            scratch.Allocate(scratchSize, mSampleFormat);
         Read(scratch.ptr(), mSampleFormat, preBlock, 0, preBufferLen, true);
         auto pFile =
            mDirManager->NewSimpleBlockFile(scratch.ptr(), preBufferLen, mSampleFormat);

         newBlock.push_back(SeqBlock(pFile, preBlock.start));
      } else {
         const SeqBlock &prepreBlock = mBlock[b0 - 1];
         const auto prepreLen = prepreBlock.f->GetLength();
         const auto sum = prepreLen + preBufferLen;

         if (!scratch.ptr())
            scratch.Allocate(scratchSize, mSampleFormat);

         Read(scratch.ptr(), mSampleFormat, prepreBlock, 0, prepreLen, true);
         Read(scratch.ptr() + prepreLen*sampleSize, mSampleFormat,
              preBlock, 0, preBufferLen, true);

         newBlock.erase(newBlock.end() - 1);
         Blockify(*mDirManager, mMaxSamples, mSampleFormat,
                  newBlock, prepreBlock.start, scratch.ptr(), sum);
      }
   }
   else {
      // The sample where we begin deletion happens to fall
      // right on the beginning of a block.
   }

   // Now, symmetrically, grab the samples in block b1 after the
   // deletion point into postBuffer.  If this is enough samples
   // for its own block, or if this would be the last block in
   // the array, write it out.  Otherwise combine it with the
   // subsequent block (splitting them 50/50 if necessary).
   const SeqBlock &postBlock = mBlock[b1];
   // start + len - 1 lies within postBlock
   const auto postBufferLen = (
       (postBlock.start + postBlock.f->GetLength()) - (start + len)
   ).as_size_t();
   if (postBufferLen) {
      if (postBufferLen >= mMinSamples || b1 == numBlocks - 1) {
         if (!scratch.ptr())
            // Last use of scratch, can ask for smaller
            scratch.Allocate(postBufferLen, mSampleFormat);
         // start + len - 1 lies within postBlock
         auto pos = (start + len - postBlock.start).as_size_t();
         Read(scratch.ptr(), mSampleFormat, postBlock, pos, postBufferLen, true);
         auto file =
            mDirManager->NewSimpleBlockFile(scratch.ptr(), postBufferLen, mSampleFormat);

         newBlock.push_back(SeqBlock(file, start));
      } else {
         SeqBlock &postpostBlock = mBlock[b1 + 1];
         const auto postpostLen = postpostBlock.f->GetLength();
         const auto sum = postpostLen + postBufferLen;

         if (!scratch.ptr())
            // Last use of scratch, can ask for smaller
            scratch.Allocate(sum, mSampleFormat);
         // start + len - 1 lies within postBlock
         auto pos = (start + len - postBlock.start).as_size_t();
         Read(scratch.ptr(), mSampleFormat, postBlock, pos, postBufferLen, true);
         Read(scratch.ptr() + (postBufferLen * sampleSize), mSampleFormat,
              postpostBlock, 0, postpostLen, true);

         Blockify(*mDirManager, mMaxSamples, mSampleFormat,
                  newBlock, start, scratch.ptr(), sum);
         b1++;
      }
   }
   else {
      // The sample where we begin deletion happens to fall
      // right on the end of a block.
   }

   // Copy the remaining blocks over from the old array
   for (i = b1 + 1; i < numBlocks; i++)
      newBlock.push_back(mBlock[i].Plus(-len));

   CommitChangesIfConsistent
      (newBlock, mNumSamples - len, wxT("Delete - branch two"));
}

void Sequence::ConsistencyCheck(const wxChar *whereStr, bool mayThrow) const
{
   ConsistencyCheck(mBlock, mMaxSamples, 0, mNumSamples, whereStr, mayThrow);
}

void Sequence::ConsistencyCheck
   (const BlockArray &mBlock, size_t maxSamples, size_t from,
    sampleCount mNumSamples, const wxChar *whereStr,
    bool mayThrow)
{
   bool bError = false;
   // Construction of the exception at the appropriate line of the function
   // gives a little more discrimination
   InconsistencyException ex;

   unsigned int numBlocks = mBlock.size();

   unsigned int i;
   sampleCount pos = from < numBlocks ? mBlock[from].start : mNumSamples;
   if ( from == 0 && pos != 0 )
      ex = CONSTRUCT_INCONSISTENCY_EXCEPTION, bError = true;

   for (i = from; !bError && i < numBlocks; i++) {
      const SeqBlock &seqBlock = mBlock[i];
      if (pos != seqBlock.start)
         ex = CONSTRUCT_INCONSISTENCY_EXCEPTION, bError = true;

      if ( seqBlock.f ) {
         const auto length = seqBlock.f->GetLength();
         if (length > maxSamples)
            ex = CONSTRUCT_INCONSISTENCY_EXCEPTION, bError = true;
         pos += length;
      }
      else
         ex = CONSTRUCT_INCONSISTENCY_EXCEPTION, bError = true;
   }
   if ( !bError && pos != mNumSamples )
      ex = CONSTRUCT_INCONSISTENCY_EXCEPTION, bError = true;

   if ( bError )
   {
      wxLogError(wxT("*** Consistency check failed after %s. ***"), whereStr);
      wxString str;
      DebugPrintf(mBlock, mNumSamples, &str);
      wxLogError(wxT("%s"), str.c_str());
      wxLogError(wxT("*** Please report this error to feedback@audacityteam.org. ***\n\n")
                 wxT("Recommended course of action:\n")
                 wxT("Undo the failed operation(s), then export or save your work and quit."));

      if (mayThrow)
         throw ex;
      else
         wxASSERT(false);
   }
}

void Sequence::CommitChangesIfConsistent
   (BlockArray &newBlock, sampleCount numSamples, const wxChar *whereStr)
{
   ConsistencyCheck( newBlock, mMaxSamples, 0, numSamples, whereStr ); // may throw

   // now commit
   // use NOFAIL-GUARANTEE

   mBlock.swap(newBlock);
   mNumSamples = numSamples;
}

void Sequence::AppendBlocksIfConsistent
(BlockArray &additionalBlocks, bool replaceLast,
 sampleCount numSamples, const wxChar *whereStr)
{
   // Any additional blocks are meant to be appended,
   // replacing the final block if there was one.

   if (additionalBlocks.empty())
      return;

   bool tmpValid = false;
   SeqBlock tmp;

   if ( replaceLast && ! mBlock.empty() ) {
      tmp = mBlock.back(), tmpValid = true;
      mBlock.pop_back();
   }

   auto prevSize = mBlock.size();

   bool consistent = false;
   auto cleanup = finally( [&] {
      if ( !consistent ) {
         mBlock.resize( prevSize );
         if ( tmpValid )
            mBlock.push_back( tmp );
      }
   } );

   std::copy( additionalBlocks.begin(), additionalBlocks.end(),
              std::back_inserter( mBlock ) );

   // Check consistency only of the blocks that were added,
   // avoiding quadratic time for repeated checking of repeating appends
   ConsistencyCheck( mBlock, mMaxSamples, prevSize, numSamples, whereStr ); // may throw

   // now commit
   // use NOFAIL-GUARANTEE

   mNumSamples = numSamples;
   consistent = true;
}

void Sequence::DebugPrintf
   (const BlockArray &mBlock, sampleCount mNumSamples, wxString *dest)
{
   unsigned int i;
   decltype(mNumSamples) pos = 0;

   for (i = 0; i < mBlock.size(); i++) {
      const SeqBlock &seqBlock = mBlock[i];
      *dest += wxString::Format
         (wxT("   Block %3u: start %8lld, len %8lld, refs %ld, "),
          i,
          seqBlock.start.as_long_long(),
          seqBlock.f ? (long long) seqBlock.f->GetLength() : 0,
          seqBlock.f ? seqBlock.f.use_count() : 0);

      if (seqBlock.f)
         *dest += seqBlock.f->GetFileName().name.GetFullName();
      else
         *dest += wxT("<missing block file>");

      if ((pos != seqBlock.start) || !seqBlock.f)
         *dest += wxT("      ERROR\n");
      else
         *dest += wxT("\n");

      if (seqBlock.f)
         pos += seqBlock.f->GetLength();
   }
   if (pos != mNumSamples)
      *dest += wxString::Format
         (wxT("ERROR mNumSamples = %lld\n"), mNumSamples.as_long_long());
}

// static
void Sequence::SetMaxDiskBlockSize(size_t bytes)
{
   sMaxDiskBlockSize = bytes;
}

size_t Sequence::GetMaxDiskBlockSize()
{
   return sMaxDiskBlockSize;
}

void Sequence::AppendBlockFile(const BlockFilePtr &blockFile)
{
   // We assume blockFile has the correct ref count already

   mBlock.push_back(SeqBlock(blockFile, mNumSamples));
   mNumSamples += blockFile->GetLength();

   // PRL:  I hoisted the intended consistency check out of the inner loop
   // See RecordingRecoveryHandler::HandleXMLEndTag

#ifdef VERY_SLOW_CHECKING
   ConsistencyCheck(wxT("AppendBlockFile"));
#endif
}
