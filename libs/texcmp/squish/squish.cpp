/* -----------------------------------------------------------------------------

	Copyright (c) 2006 Simon Brown                          si@sjbrown.co.uk

	Permission is hereby granted, free of charge, to any person obtaining
	a copy of this software and associated documentation files (the
	"Software"), to	deal in the Software without restriction, including
	without limitation the rights to use, copy, modify, merge, publish,
	distribute, sublicense, and/or sell copies of the Software, and to
	permit persons to whom the Software is furnished to do so, subject to
	the following conditions:

	The above copyright notice and this permission notice shall be included
	in all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
	OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
	CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
	TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   -------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdint.h>

#include "squish.h"
#include "colourset.h"
#include "maths.h"
#include "rangefit.h"
#include "clusterfit.h"
#include "colourblock.h"
#include "alpha.h"
#include "singlecolourfit.h"
#include "singlecolourfitfast.h"
#include "twocolourfitfast.h"
#include <wx/thread.h>

extern bool g_throttle_squish;

namespace squish {

static int FixFlags( int flags )
{
	// grab the flag bits
	int method = flags & ( kDxt1 | kDxt3 | kDxt5 );
	int fit = flags & ( kColourIterativeClusterFit | kColourClusterFit | kColourRangeFit );
	int metric = flags & ( kColourMetricPerceptual | kColourMetricUniform );
	int extra = flags & kWeightColourByAlpha;

	// set defaults
	if( method != kDxt3 && method != kDxt5 )
		method = kDxt1;
	if( fit != kColourRangeFit && fit != kColourIterativeClusterFit)
		fit = kColourClusterFit;
	if( metric != kColourMetricUniform )
		metric = kColourMetricPerceptual;

	// done
	return method | fit | metric | extra;
}

void Compress_dxt1( u8 const* rgba, void* block, int flags )
{
	// get the block locations
	void* colourBlock = block;

	// create the minimal point set
	ColourSet colours( rgba, flags );

	// check the compression type and compress colour

	if( colours.GetCount() == 1)
	{
		// always do a single colour fit
		SingleColourFitFast fit( &colours, flags );
		fit.Compress3( colourBlock );
	}
	else if( colours.GetCount() == 2)
	{
                TwoColourFitFast fit( &colours, flags );
		fit.Compress3( colourBlock );
	}
	else if( ( flags & kColourRangeFit ) != 0 || colours.GetCount() <= 4 )
	{
		// do a range fit
		RangeFit fit( &colours, flags );
		fit.Compress3( colourBlock );
	}
	else
	{
		// default to a cluster fit (could be iterative or not)
		ClusterFit fit( &colours, flags );
		fit.Compress3( colourBlock );
	}
}

void Compress( u8 const* rgba, void* block, int flags )
{
	// compress with full mask
	CompressMasked( rgba, 0xffff, block, flags );
}

void CompressMasked( u8 const* rgba, int mask, void* block, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// get the block locations
	void* colourBlock = block;
	void* alphaBock = block;
	if( ( flags & ( kDxt3 | kDxt5 ) ) != 0 )
		colourBlock = reinterpret_cast< u8* >( block ) + 8;

	// create the minimal point set
	ColourSet colours( rgba, mask, flags );

	// check the compression type and compress colour
	if( colours.GetCount() == 1 )
	{
		// always do a single colour fit
		SingleColourFit fit( &colours, flags );
		fit.Compress( colourBlock );
	}
	else if( ( flags & kColourRangeFit ) != 0 || colours.GetCount() == 0 )
	{
		// do a range fit
		RangeFit fit( &colours, flags );
		fit.Compress( colourBlock );
	}
	else
	{
		// default to a cluster fit (could be iterative or not)
		ClusterFit fit( &colours, flags );
		fit.Compress( colourBlock );
	}

	// compress alpha separately if necessary
	if( ( flags & kDxt3 ) != 0 )
		CompressAlphaDxt3( rgba, mask, alphaBock );
	else if( ( flags & kDxt5 ) != 0 )
		CompressAlphaDxt5( rgba, mask, alphaBock );
}

void Decompress( u8* rgba, void const* block, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// get the block locations
	void const* colourBlock = block;
	void const* alphaBock = block;
	if( ( flags & ( kDxt3 | kDxt5 ) ) != 0 )
		colourBlock = reinterpret_cast< u8 const* >( block ) + 8;

	// decompress colour
	DecompressColour( rgba, colourBlock, ( flags & kDxt1 ) != 0 );

	// decompress alpha separately if necessary
	if( ( flags & kDxt3 ) != 0 )
		DecompressAlphaDxt3( rgba, alphaBock );
	else if( ( flags & kDxt5 ) != 0 )
		DecompressAlphaDxt5( rgba, alphaBock );
}

int GetStorageRequirements( int width, int height, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// compute the storage requirements
	int blockcount = ( ( width + 3 )/4 ) * ( ( height + 3 )/4 );
	int blocksize = ( ( flags & kDxt1 ) != 0 ) ? 8 : 16;
	return blockcount*blocksize;
}

void CompressImage( u8 const* rgba, int width, int height, void* blocks, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// initialise the block output
	u8* targetBlock = reinterpret_cast< u8* >( blocks );
	int bytesPerBlock = ( ( flags & kDxt1 ) != 0 ) ? 8 : 16;

	// loop over blocks
	for( int y = 0; y < height; y += 4 )
	{
		for( int x = 0; x < width; x += 4 )
		{
			// build the 4x4 block of pixels
			u8 sourceRgba[16*4];
			u8* targetPixel = sourceRgba;
			int mask = 0;
			for( int py = 0; py < 4; ++py )
			{
				for( int px = 0; px < 4; ++px )
				{
					// get the source pixel in the image
					int sx = x + px;
					int sy = y + py;

					// enable if we're in the image
					if( sx < width && sy < height )
					{
						// copy the rgba value
						u8 const* sourcePixel = rgba + 4*( width*sy + sx );
						for( int i = 0; i < 4; ++i )
							*targetPixel++ = *sourcePixel++;

						// enable this pixel
						mask |= ( 1 << ( 4*py + px ) );
					}
					else
					{
						// skip this pixel as its outside the image
						targetPixel += 4;
					}
				}
			}

			// compress it into the output
			CompressMasked( sourceRgba, mask, targetBlock, flags );

			// advance
			targetBlock += bytesPerBlock;
		}
	}
}

void CompressImageRGB( u8 const* rgb, int width, int height, void* blocks, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// initialise the block output
	u8* targetBlock = reinterpret_cast< u8* >( blocks );
	int bytesPerBlock = ( ( flags & kDxt1 ) != 0 ) ? 8 : 16;

	// loop over blocks
	for( int y = 0; y < height; y += 4 )
	{
		for( int x = 0; x < width; x += 4 )
		{
			// build the 4x4 block of pixels
			u8 sourceRgba[16*4];
			u8* targetPixel = sourceRgba;
			int mask = 0;
			for( int py = 0; py < 4; ++py )
			{
				for( int px = 0; px < 4; ++px )
				{
					// get the source pixel in the image
					int sx = x + px;
					int sy = y + py;

					// enable if we're in the image
					if( sx < width && sy < height )
					{
						// copy the rgba value
						u8 const* sourcePixel = rgb + 3*( width*sy + sx );
						for( int i = 0; i < 3; ++i )
                                                   *targetPixel++ = *sourcePixel++;
                                                *targetPixel++ = 255;

						// enable this pixel
						mask |= ( 1 << ( 4*py + px ) );
					}
					else
					{
						// skip this pixel as its outside the image
						targetPixel += 4;
					}
				}
			}

			// compress it into the output
			CompressMasked( sourceRgba, mask, targetBlock, flags );

			// advance
			targetBlock += bytesPerBlock;
		}
        }
}

void CompressImageRGBpow2_Flatten_Throttle_Abort( u8 const* rgb, int width, int height, void* blocks, int flags,
                                                  bool b_flatten, void (*throttle)(void*), void *throttle_data, volatile bool &b_abort )
{
    // fix any bad flags
    flags = FixFlags( flags );

    // initialise the block output
    u8* targetBlock = reinterpret_cast< u8* >( blocks );
    int bytesPerBlock = ( ( flags & kDxt1 ) != 0 ) ? 8 : 16;

    u8 r_flat_mask = 0xff;
    u8 g_flat_mask = 0xff;
    u8 b_flat_mask = 0xff;

    if(b_flatten){
        r_flat_mask = 0xf8;
        g_flat_mask = 0xfc;
        b_flat_mask = 0xf8;
    }

    // loop over blocks
    double tt = 0;

    int bw = std::min(width, 4);
    int bh = std::min(height, 4);

    for( int y = 0; y < height; y += 4 )
    {
        for( int x = 0; x < width; x += 4 )
        {
            // build the 4x4 block of pixels
            u8 sourceRgba[16*4];
            u8* targetPixel = sourceRgba;

            for( int py = 0; py < 4; ++py )
            {
                for( int px = 0; px < 4; ++px )
                {
                    // get the source pixel in the image
                    int sx = x + (px % bw);
                    int sy = y + (py % bh);

                    // copy the rgba value
                    u8 const* sourcePixel = rgb + 3*( width*sy + sx );

                    *targetPixel++ = sourcePixel[0] & r_flat_mask;
                    *targetPixel++ = sourcePixel[1] & g_flat_mask;
                    *targetPixel++ = sourcePixel[2] & b_flat_mask;
                    *targetPixel++ = 255;
                }
            }

            // compress it into the output
            Compress_dxt1( sourceRgba, targetBlock, flags );
//            Compress( sourceRgba, targetBlock, flags );

            // advance
            targetBlock += bytesPerBlock;
        }
        if( throttle )
            throttle(throttle_data);

        if( b_abort)
            break;
    }
}


void DecompressImage( u8* rgba, int width, int height, void const* blocks, int flags )
{
	// fix any bad flags
	flags = FixFlags( flags );

	// initialise the block input
	u8 const* sourceBlock = reinterpret_cast< u8 const* >( blocks );
	int bytesPerBlock = ( ( flags & kDxt1 ) != 0 ) ? 8 : 16;

	// loop over blocks
	for( int y = 0; y < height; y += 4 )
	{
		for( int x = 0; x < width; x += 4 )
		{
			// decompress the block
			u8 targetRgba[4*16];
			Decompress( targetRgba, sourceBlock, flags );

			// write the decompressed pixels to the correct image locations
			u8 const* sourcePixel = targetRgba;
			for( int py = 0; py < 4; ++py )
			{
				for( int px = 0; px < 4; ++px )
				{
					// get the target location
					int sx = x + px;
					int sy = y + py;
					if( sx < width && sy < height )
					{
						u8* targetPixel = rgba + 4*( width*sy + sx );

						// copy the rgba value
						for( int i = 0; i < 4; ++i )
							*targetPixel++ = *sourcePixel++;
					}
					else
					{
						// skip this pixel as its outside the image
						sourcePixel += 4;
					}
				}
			}

			// advance
			sourceBlock += bytesPerBlock;
		}
	}
}

} // namespace squish
